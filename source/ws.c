#include "ws.h"

#include <3ds.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// Not all newlib configurations define this.
#ifndef INADDR_NONE
#define INADDR_NONE ((in_addr_t)0xFFFFFFFFUL)
#endif

#define SOC_ALIGN      0x1000
#define SOC_BUFFERSIZE 0x100000

#define RX_CAP  32768
#define MSG_CAP 32768
#define REQ_CAP 1024

static u32* s_socBuf = NULL;
static bool s_socReady = false;

struct WsConn {
	int      fd;
	WsState  state;
	char     err[128];

	char     host[128];
	int      port;
	char     path[192];

	struct sockaddr_in addr;

	char     req[REQ_CAP];
	int      reqLen;
	int      reqSent;

	char     hdr[2048];
	int      hdrLen;

	unsigned char rx[RX_CAP];
	int      rxLen;

	char     msg[MSG_CAP]; // reassembly across continuation frames
	int      msgLen;
	int      msgOpcode;    // opcode of the fragment run in progress

	u64      deadline;     // handshake watchdog, ms since epoch
};

// ---------------------------------------------------------------- utilities

// Closes a socket with SO_LINGER(0) so it dies with an RST instead of
// lingering in TIME_WAIT. The 3DS SOC's socket table is tiny, and every
// server-closed request would otherwise occupy a slot for minutes until
// new connects stall.
static void close_rst(int fd);

static void fail(WsConn* c, const char* what)
{
	if (c->state != WSS_ERR) {
		snprintf(c->err, sizeof(c->err), "%s", what);
		c->state = WSS_ERR;
	}
	if (c->fd >= 0) {
		close_rst(c->fd);
		c->fd = -1;
	}
}

static bool would_block(void)
{
	return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY;
}

static void b64_encode(const unsigned char* in, int len, char* out)
{
	static const char T[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int o = 0;
	for (int i = 0; i < len; i += 3) {
		int rem = len - i;
		unsigned v = (unsigned)in[i] << 16;
		if (rem > 1) v |= (unsigned)in[i + 1] << 8;
		if (rem > 2) v |= (unsigned)in[i + 2];
		out[o++] = T[(v >> 18) & 0x3F];
		out[o++] = T[(v >> 12) & 0x3F];
		out[o++] = rem > 1 ? T[(v >> 6) & 0x3F] : '=';
		out[o++] = rem > 2 ? T[v & 0x3F] : '=';
	}
	out[o] = '\0';
}

// ------------------------------------------------------------------ SOC init

bool net_init(void)
{
	if (s_socReady) return true;
	s_socBuf = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
	if (!s_socBuf) return false;
	if (R_FAILED(socInit(s_socBuf, SOC_BUFFERSIZE))) {
		free(s_socBuf);
		s_socBuf = NULL;
		return false;
	}
	s_socReady = true;
	return true;
}

void net_exit(void)
{
	if (!s_socReady) return;
	socExit();
	// The SOC buffer is deliberately leaked: socExit() can return before the
	// service has finished with the shared memory, and freeing it early has
	// been observed to corrupt the heap on hardware.
	s_socReady = false;
}

// ------------------------------------------------------------------- connect

// Resolves a dotted-quad or hostname. Returns false on failure.
static bool resolve(WsConn* c)
{
	memset(&c->addr, 0, sizeof(c->addr));
	c->addr.sin_family = AF_INET;
	c->addr.sin_port   = htons((unsigned short)c->port);

	in_addr_t ip = inet_addr(c->host);
	if (ip != INADDR_NONE) {
		c->addr.sin_addr.s_addr = ip;
		return true;
	}
	// Only hostnames reach the (blocking) resolver; plain IPs skip it.
	struct hostent* he = gethostbyname(c->host);
	if (!he || !he->h_addr_list || !he->h_addr_list[0]) return false;
	memcpy(&c->addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
	return true;
}

WsConn* ws_begin(const char* host, int port, const char* path)
{
	WsConn* c = (WsConn*)calloc(1, sizeof(WsConn));
	if (!c) return NULL;

	c->fd    = -1;
	c->state = WSS_RESOLVE;
	c->port  = port;
	c->msgOpcode = -1;
	snprintf(c->host, sizeof(c->host), "%s", host);
	snprintf(c->path, sizeof(c->path), "%s", path);
	c->deadline = osGetTime() + 15000;

	if (!s_socReady) {
		fail(c, "Network not initialised");
		return c;
	}
	return c;
}

static void build_request(WsConn* c)
{
	unsigned char key[16];
	for (int i = 0; i < 16; i++) key[i] = (unsigned char)(rand() & 0xFF);
	char keyb64[32];
	b64_encode(key, 16, keyb64);

	c->reqLen = snprintf(c->req, sizeof(c->req),
	    "GET %s HTTP/1.1\r\n"
	    "Host: %s:%d\r\n"
	    "Upgrade: websocket\r\n"
	    "Connection: Upgrade\r\n"
	    "Sec-WebSocket-Key: %s\r\n"
	    "Sec-WebSocket-Version: 13\r\n"
	    "Origin: http://%s:%d\r\n"
	    "User-Agent: QuizFabric3DS/1.0\r\n"
	    "\r\n",
	    c->path, c->host, c->port, keyb64, c->host, c->port);
	c->reqSent = 0;
}

static void step_resolve(WsConn* c)
{
	if (!resolve(c)) {
		fail(c, "Cannot resolve host");
		return;
	}
	c->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (c->fd < 0) {
		fail(c, "socket() failed");
		return;
	}
	int flags = fcntl(c->fd, F_GETFL, 0);
	fcntl(c->fd, F_SETFL, flags | O_NONBLOCK);

	int r = connect(c->fd, (struct sockaddr*)&c->addr, sizeof(c->addr));
	if (r == 0) {
		build_request(c);
		c->state = WSS_SEND_REQ;
		return;
	}
	if (!would_block()) {
		fail(c, "Connection refused");
		return;
	}
	c->state = WSS_CONNECT;
}

static void step_connect(WsConn* c)
{
	fd_set wr, ex;
	FD_ZERO(&wr);
	FD_ZERO(&ex);
	FD_SET(c->fd, &wr);
	FD_SET(c->fd, &ex);
	struct timeval tv = { 0, 0 };

	int r = select(c->fd + 1, NULL, &wr, &ex, &tv);
	if (r < 0) {
		fail(c, "select() failed");
		return;
	}
	if (r == 0) return; // still pending

	if (FD_ISSET(c->fd, &ex)) {
		fail(c, "Connection refused");
		return;
	}
	// Writable. Confirm there is no pending socket error before proceeding.
	// ETXTBSY (+/-26) is reported spuriously by the 3DS SOC driver on fine
	// connections, so it must be ignored here and in http_get_step().
	int soerr = 0;
	socklen_t slen = sizeof(soerr);
	if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr != 0
	    && soerr != 26 && soerr != -26) {
		fail(c, "Connection refused");
		return;
	}
	build_request(c);
	c->state = WSS_SEND_REQ;
}

static void step_send_req(WsConn* c)
{
	while (c->reqSent < c->reqLen) {
		int n = send(c->fd, c->req + c->reqSent, c->reqLen - c->reqSent, 0);
		if (n > 0) {
			c->reqSent += n;
			continue;
		}
		if (n < 0 && would_block()) return; // retry next frame
		fail(c, "Send failed during handshake");
		return;
	}
	c->hdrLen = 0;
	c->state  = WSS_HDRS;
}

static void step_hdrs(WsConn* c)
{
	for (;;) {
		if (c->hdrLen >= (int)sizeof(c->hdr) - 1) {
			fail(c, "Handshake response too large");
			return;
		}
		int n = recv(c->fd, c->hdr + c->hdrLen, sizeof(c->hdr) - 1 - c->hdrLen, 0);
		if (n == 0) {
			fail(c, "Server closed during handshake");
			return;
		}
		if (n < 0) {
			if (would_block()) return; // resume next frame
			fail(c, "Recv failed during handshake");
			return;
		}
		c->hdrLen += n;
		c->hdr[c->hdrLen] = '\0';

		char* end = strstr(c->hdr, "\r\n\r\n");
		if (!end) continue; // need more

		if (!strstr(c->hdr, " 101")) {
			// Surface the status line, which is far more useful than "failed".
			char line[80];
			int i = 0;
			while (i < (int)sizeof(line) - 1 && c->hdr[i] && c->hdr[i] != '\r') {
				line[i] = c->hdr[i];
				i++;
			}
			line[i] = '\0';
			char msg[128];
			snprintf(msg, sizeof(msg), "Upgrade refused: %s", line);
			fail(c, msg);
			return;
		}

		// Any bytes past the header terminator are already WebSocket frames.
		char* body    = end + 4;
		int   bodyLen = c->hdrLen - (int)(body - c->hdr);
		if (bodyLen > 0) {
			if (bodyLen > RX_CAP) {
				fail(c, "Receive overflow");
				return;
			}
			memcpy(c->rx, body, bodyLen);
			c->rxLen = bodyLen;
		}
		c->state = WSS_OPEN;
		return;
	}
}

// Drains the socket into the rx ring. Returns false if the peer went away.
static bool pump(WsConn* c)
{
	for (;;) {
		if (c->rxLen >= RX_CAP) return true; // full; parse before reading more
		int n = recv(c->fd, c->rx + c->rxLen, RX_CAP - c->rxLen, 0);
		if (n > 0) {
			c->rxLen += n;
			continue;
		}
		if (n == 0) {
			c->state = WSS_CLOSED;
			return false;
		}
		if (would_block()) return true;
		fail(c, "Connection lost");
		return false;
	}
}

WsState ws_step(WsConn* c)
{
	if (!c) return WSS_ERR;

	if (c->state < WSS_OPEN && osGetTime() > c->deadline) {
		fail(c, "Connection timed out");
		return c->state;
	}

	switch (c->state) {
	case WSS_RESOLVE:  step_resolve(c);  break;
	case WSS_CONNECT:  step_connect(c);  break;
	case WSS_SEND_REQ: step_send_req(c); break;
	case WSS_HDRS:     step_hdrs(c);     break;
	case WSS_OPEN:     pump(c);          break;
	default: break;
	}
	return c->state;
}

WsState ws_state(WsConn* c) { return c ? c->state : WSS_ERR; }

const char* ws_error(WsConn* c) { return (c && c->err[0]) ? c->err : "Unknown error"; }

// -------------------------------------------------------------- frame output

static bool send_all(WsConn* c, const unsigned char* buf, int len)
{
	int sent = 0;
	// The socket is non-blocking; spin briefly rather than dropping the frame.
	// Frames here are small (a few hundred bytes at most).
	u64 giveUp = osGetTime() + 3000;
	while (sent < len) {
		int n = send(c->fd, buf + sent, len - sent, 0);
		if (n > 0) {
			sent += n;
			continue;
		}
		if (n < 0 && would_block()) {
			if (osGetTime() > giveUp) {
				fail(c, "Send timed out");
				return false;
			}
			svcSleepThread(1000000ULL); // 1 ms
			continue;
		}
		fail(c, "Send failed");
		return false;
	}
	return true;
}

static bool send_frame(WsConn* c, int opcode, const char* data, int len)
{
	if (!c || c->state != WSS_OPEN) return false;

	unsigned char hdr[14];
	int h = 0;
	hdr[h++] = (unsigned char)(0x80 | (opcode & 0x0F)); // FIN + opcode

	if (len < 126) {
		hdr[h++] = (unsigned char)(0x80 | len);
	} else if (len < 65536) {
		hdr[h++] = 0x80 | 126;
		hdr[h++] = (unsigned char)((len >> 8) & 0xFF);
		hdr[h++] = (unsigned char)(len & 0xFF);
	} else {
		hdr[h++] = 0x80 | 127;
		for (int i = 0; i < 4; i++) hdr[h++] = 0; // high 32 bits: always 0 here
		hdr[h++] = (unsigned char)((len >> 24) & 0xFF);
		hdr[h++] = (unsigned char)((len >> 16) & 0xFF);
		hdr[h++] = (unsigned char)((len >> 8) & 0xFF);
		hdr[h++] = (unsigned char)(len & 0xFF);
	}

	unsigned char mask[4];
	for (int i = 0; i < 4; i++) {
		mask[i] = (unsigned char)(rand() & 0xFF);
		hdr[h++] = mask[i];
	}
	if (!send_all(c, hdr, h)) return false;

	// Mask in chunks so a large payload does not need a full-size scratch copy.
	unsigned char chunk[512];
	int off = 0;
	while (off < len) {
		int n = len - off;
		if (n > (int)sizeof(chunk)) n = (int)sizeof(chunk);
		for (int i = 0; i < n; i++) {
			chunk[i] = (unsigned char)data[off + i] ^ mask[(off + i) & 3];
		}
		if (!send_all(c, chunk, n)) return false;
		off += n;
	}
	return true;
}

bool ws_send_text(WsConn* c, const char* data, int len)
{
	return send_frame(c, 0x1, data, len);
}

// --------------------------------------------------------------- frame input

static void rx_consume(WsConn* c, int n)
{
	if (n >= c->rxLen) {
		c->rxLen = 0;
		return;
	}
	memmove(c->rx, c->rx + n, c->rxLen - n);
	c->rxLen -= n;
}

int ws_poll(WsConn* c, char* out, int outsz, int* outlen)
{
	if (!c) return -1;
	if (c->state == WSS_ERR || c->state == WSS_CLOSED) return -1;
	if (c->state != WSS_OPEN) return 0;

	if (!pump(c)) return -1;

	for (;;) {
		if (c->rxLen < 2) return 0;

		unsigned char b0 = c->rx[0];
		unsigned char b1 = c->rx[1];
		int fin    = (b0 & 0x80) != 0;
		int opcode = b0 & 0x0F;
		int masked = (b1 & 0x80) != 0; // servers must not mask
		long long plen = b1 & 0x7F;
		int pos = 2;

		if (plen == 126) {
			if (c->rxLen < 4) return 0;
			plen = ((long long)c->rx[2] << 8) | c->rx[3];
			pos = 4;
		} else if (plen == 127) {
			if (c->rxLen < 10) return 0;
			plen = 0;
			for (int i = 0; i < 8; i++) plen = (plen << 8) | c->rx[2 + i];
			pos = 10;
		}
		if (masked) pos += 4; // tolerate it, though it should not happen

		if (plen < 0 || plen > MSG_CAP) {
			fail(c, "Frame too large");
			return -1;
		}
		if (c->rxLen < pos + plen) {
			// Incomplete frame. If it cannot ever fit, the buffer is too small.
			if (pos + plen > RX_CAP) {
				fail(c, "Frame exceeds buffer");
				return -1;
			}
			return 0;
		}

		unsigned char* payload = c->rx + pos;
		if (masked) {
			unsigned char* mk = c->rx + pos - 4;
			for (long long i = 0; i < plen; i++) payload[i] ^= mk[i & 3];
		}

		if (opcode == 0x8) { // close
			rx_consume(c, pos + (int)plen);
			send_frame(c, 0x8, "", 0);
			c->state = WSS_CLOSED;
			return -1;
		}
		if (opcode == 0x9) { // ping -> pong with same payload
			send_frame(c, 0xA, (const char*)payload, (int)plen);
			rx_consume(c, pos + (int)plen);
			continue;
		}
		if (opcode == 0xA) { // pong
			rx_consume(c, pos + (int)plen);
			continue;
		}

		// Data frame (0x1 text, 0x2 binary, 0x0 continuation).
		if (opcode == 0x0) {
			if (c->msgOpcode < 0) { // continuation with nothing to continue
				rx_consume(c, pos + (int)plen);
				continue;
			}
		} else {
			c->msgOpcode = opcode;
			c->msgLen    = 0;
		}

		if (c->msgLen + plen > MSG_CAP) {
			fail(c, "Message too large");
			return -1;
		}
		memcpy(c->msg + c->msgLen, payload, (size_t)plen);
		c->msgLen += (int)plen;
		rx_consume(c, pos + (int)plen);

		if (!fin) continue; // more fragments to come

		int  kind = c->msgOpcode;
		int  len  = c->msgLen;
		c->msgOpcode = -1;
		c->msgLen    = 0;

		if (kind != 0x1) continue; // ignore binary payloads

		int n = len < outsz - 1 ? len : outsz - 1;
		memcpy(out, c->msg, (size_t)n);
		out[n] = '\0';
		if (outlen) *outlen = n;
		return 1;
	}
}

// Closes a socket with SO_LINGER(0) so it dies with an RST instead of
// lingering in TIME_WAIT. The 3DS SOC's socket table is tiny, and every
// server-closed request would otherwise occupy a slot for minutes until
// new connects stall.
static void close_rst(int fd)
{
	struct linger lg;
	lg.l_onoff = 1;
	lg.l_linger = 0;
	setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
	closesocket(fd);
}

void ws_close(WsConn* c)
{
	if (!c) return;
	if (c->fd >= 0) {
		if (c->state == WSS_OPEN) send_frame(c, 0x8, "", 0);
		close_rst(c->fd);
		c->fd = -1;
	}
	free(c);
}

// ------------------------------------------------------------------ HTTP GET

// Waits until fd is readable (forWrite=false) or writable, or the deadline
// passes. Returns false on timeout or hard error.
static bool wait_fd(int fd, bool forWrite, u64 deadline)
{
	while (osGetTime() < deadline) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		int left = (int)(deadline - osGetTime());
		struct timeval tv = { left / 1000, (left % 1000) * 1000 };
		int r = forWrite ? select(fd + 1, NULL, &fds, NULL, &tv)
		                 : select(fd + 1, &fds, NULL, NULL, &tv);
		if (r > 0) return true;
		if (r < 0 && errno != EINTR) return false;
		// r == 0 or EINTR: loop again with the remaining time.
	}
	return false;
}

char* http_get(const char* host, int port, const char* path, int timeout_ms)
{
	if (!s_socReady) return NULL;

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons((unsigned short)port);

	in_addr_t ip = inet_addr(host);
	if (ip != INADDR_NONE) {
		sa.sin_addr.s_addr = ip;
	} else {
		struct hostent* he = gethostbyname(host);
		if (!he || !he->h_addr_list || !he->h_addr_list[0]) return NULL;
		memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
	}

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return NULL;

	// libctru has no SO_RCVTIMEO / SO_SNDTIMEO, so the timeout is driven with
	// select() on a non-blocking socket, as the handshake machine does.
	u64 deadline = osGetTime() + (u64)timeout_ms;

	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	int r = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
	if (r < 0 && !would_block()) {
		close_rst(fd);
		return NULL;
	}
	if (r != 0 && !wait_fd(fd, true, deadline)) {
		close_rst(fd);
		return NULL;
	}
	// Writable. Confirm there is no pending socket error before proceeding.
	// ETXTBSY (+/-26) is a 3DS SOC driver false positive; ignore it.
	int soerr = 0;
	socklen_t slen = sizeof(soerr);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr != 0
	    && soerr != 26 && soerr != -26) {
		close_rst(fd);
		return NULL;
	}

	char req[512];
	int reqLen = snprintf(req, sizeof(req),
	    "GET %s HTTP/1.1\r\n"
	    "Host: %s:%d\r\n"
	    "User-Agent: QuizFabric3DS/1.0\r\n"
	    "Connection: close\r\n"
	    "\r\n",
	    path, host, port);

	int sent = 0;
	while (sent < reqLen) {
		int n = send(fd, req + sent, reqLen - sent, 0);
		if (n > 0) {
			sent += n;
			continue;
		}
		if (n < 0 && would_block()) {
			if (!wait_fd(fd, true, deadline)) {
				close_rst(fd);
				return NULL;
			}
			continue;
		}
		close_rst(fd);
		return NULL;
	}

	int   cap = 8192;
	int   len = 0;
	char* buf = (char*)malloc(cap);
	if (!buf) {
		close_rst(fd);
		return NULL;
	}
	for (;;) {
		if (len + 1024 > cap) {
			int   ncap = cap * 2;
			char* nb   = (char*)realloc(buf, ncap);
			if (!nb) break;
			buf = nb;
			cap = ncap;
		}
		int n = recv(fd, buf + len, cap - len - 1, 0);
		if (n > 0) {
			len += n;
			continue;
		}
		if (n == 0) break; // server closed
		if (would_block()) {
			// Timeout: parse whatever arrived so far, as before.
			if (!wait_fd(fd, false, deadline)) break;
			continue;
		}
		break; // hard error
	}
	close_rst(fd);
	buf[len] = '\0';

	char* body = strstr(buf, "\r\n\r\n");
	if (!body) {
		free(buf);
		return NULL;
	}
	body += 4;

	// Chunked responses would need de-chunking; Flask sends this one with a
	// plain Content-Length, so a straight copy of the body is correct.
	char* out = strdup(body);
	free(buf);
	return out;
}

// ------------------------------------------------------- non-blocking HTTP GET

enum { HG_CONNECT, HG_SEND, HG_RECV, HG_DONE };

struct HttpGet {
	int  fd;
	int  st;
	u64  deadline;
	int  timeoutMs;   // stall timeout: every received chunk extends deadline
	char host[128];
	char path[256];
	int  port;
	struct sockaddr_in sa;
	char req[1280];
	int  reqLen, reqSent;
	char* buf;
	int   cap, len;
	char  stage[20]; // filled when a step fails
	bool  isPost;    // POST instead of GET
	char  body[512]; // urlencoded form body
	int   contentLen; // from the Content-Length header; -1 until known
	// File-download mode (http_get_begin_file): the body streams straight
	// to this FILE instead of the RAM buffer, resuming at `offset` bytes
	// via a Range header.
	FILE* file;
	char  fpath[320];
	int   offset;     // resume offset (bytes already in the file)
	int   rangeOff;   // the Range offset sent in the request
	int   status;     // HTTP status code, -1 until the headers are parsed
	int   total;      // offset + contentLen when known
	bool  hdrsDone;
	char  chunk[4096];
};

static const char* s_beginErr = "";

const char* http_begin_error(void) { return s_beginErr; }
const char* http_get_stage(HttpGet* h) { return (h && h->stage[0]) ? h->stage : "unknown"; }

HttpGet* http_get_begin(const char* host, int port, const char* path, int timeout_ms)
{
	s_beginErr = "";
	if (!s_socReady) {
		s_beginErr = "no soc";
		return NULL;
	}

	HttpGet* h = (HttpGet*)calloc(1, sizeof(HttpGet));
	if (!h) {
		s_beginErr = "oom";
		return NULL;
	}

	h->fd = -1;
	h->deadline = osGetTime() + (u64)timeout_ms;
	h->timeoutMs = timeout_ms;
	h->port = port;
	h->contentLen = -1;
	h->status = -1;
	snprintf(h->host, sizeof(h->host), "%s", host);
	snprintf(h->path, sizeof(h->path), "%s", path);

	memset(&h->sa, 0, sizeof(h->sa));
	h->sa.sin_family = AF_INET;
	h->sa.sin_port   = htons((unsigned short)port);

	in_addr_t ip = inet_addr(host);
	if (ip != INADDR_NONE) {
		h->sa.sin_addr.s_addr = ip;
	} else {
		// Hostnames hit the (blocking) resolver once, here at begin().
		struct hostent* he = gethostbyname(host);
		if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
			s_beginErr = "dns";
			free(h);
			return NULL;
		}
		memcpy(&h->sa.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
	}

	h->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (h->fd < 0) {
		s_beginErr = "socket";
		free(h);
		return NULL;
	}
	int flags = fcntl(h->fd, F_GETFL, 0);
	fcntl(h->fd, F_SETFL, flags | O_NONBLOCK);

	int r = connect(h->fd, (struct sockaddr*)&h->sa, sizeof(h->sa));
	if (r < 0 && !would_block()) {
		s_beginErr = "connect";
		close_rst(h->fd);
		free(h);
		return NULL;
	}
	h->st = HG_CONNECT;
	if (r == 0) h->st = HG_SEND;
	return h;
}

// One non-blocking readiness probe: select with a zero timeout, so a frame
// never stalls on the network.
static bool poll_fd(int fd, bool forWrite)
{
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);
	struct timeval tv = { 0, 0 };
	int r = forWrite ? select(fd + 1, NULL, &fds, NULL, &tv)
	                 : select(fd + 1, &fds, NULL, NULL, &tv);
	return r > 0;
}

float http_get_progress(HttpGet* h)
{
	if (!h) return -1.0f;
	if (h->st == HG_DONE) return 1.0f;
	if (h->st != HG_RECV) return -1.0f;
	if (h->file) {
		// File mode: bytes already on disk + bytes received this session,
		// over the full body size when the server told us.
		if (h->total > 0 && h->hdrsDone) {
			float p = (float)(h->offset + h->len) / (float)h->total;
			if (p < 0.0f) p = 0.0f;
			if (p > 1.0f) p = 1.0f;
			return p;
		}
		return -1.0f;
	}
	if (h->contentLen <= 0) return -1.0f;
	float p = (float)h->len / (float)h->contentLen;
	if (p < 0.0f) p = 0.0f;
	if (p > 1.0f) p = 1.0f;
	return p;
}

// Streams a GET body straight to `file`, resuming at `offset` bytes (a
// Range header is sent; the server may restart from 0 instead). The file
// must already be opened for append/read-write; `fpath` is its path so the
// restart-from-0 case can reopen it truncated. Drive with http_get_step.
HttpGet* http_get_begin_file(const char* host, int port, const char* path,
                             int timeout_ms, FILE* file, int offset,
                             const char* fpath)
{
	HttpGet* h = http_get_begin(host, port, path, timeout_ms);
	if (!h) return NULL;
	h->file = file;
	h->offset = offset;
	h->rangeOff = offset;
	snprintf(h->fpath, sizeof(h->fpath), "%s", fpath ? fpath : "");
	return h;
}

int http_get_status(HttpGet* h)
{
	return h ? h->status : -1;
}

// Bytes of the body received so far (file mode: including the resume base).
int http_get_downloaded(HttpGet* h)
{
	if (!h) return 0;
	if (h->file) return h->offset + h->len;
	return h->len;
}

// ------------------------------------------------------------------ ping
// Reachability check for offline mode (ICMP is unavailable on the 3DS SOC,
// so this is a non-blocking TCP connect to 8.8.8.8:53 instead). Any answer
// - even a refusal - proves the network is up; 15 seconds of silence means
// offline.

static int  s_pingFd = -1;
static u64  s_pingDeadline = 0;
static int  s_pingState = 0; // 0 idle, 1 checking, 2 online, 3 no internet

int net_ping_state(void) { return s_pingState; }

void net_ping_start(void)
{
	if (s_pingFd >= 0) {
		close_rst(s_pingFd);
		s_pingFd = -1;
	}
	s_pingState = 1;
	s_pingDeadline = osGetTime() + 15000;
	if (!s_socReady) {
		s_pingState = 3;
		return;
	}
	s_pingFd = socket(AF_INET, SOCK_STREAM, 0);
	if (s_pingFd < 0) {
		s_pingState = 3;
		return;
	}
	fcntl(s_pingFd, F_SETFL, fcntl(s_pingFd, F_GETFL, 0) | O_NONBLOCK);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(53);
	sa.sin_addr.s_addr = inet_addr("8.8.8.8");
	int r = connect(s_pingFd, (struct sockaddr*)&sa, sizeof(sa));
	if (r != 0 && errno != EINPROGRESS && !would_block()) {
		close_rst(s_pingFd);
		s_pingFd = -1;
		s_pingState = 3;
	}
}

void net_ping_step(void)
{
	if (s_pingState != 1) return;
	if (osGetTime() >= s_pingDeadline) {
		// No response in 15 seconds: there is no real connection.
		if (s_pingFd >= 0) {
			close_rst(s_pingFd);
			s_pingFd = -1;
		}
		s_pingState = 3;
		return;
	}
	if (s_pingFd < 0) {
		s_pingState = 3;
		return;
	}
	fd_set wfds, xfds;
	FD_ZERO(&wfds);
	FD_ZERO(&xfds);
	FD_SET(s_pingFd, &wfds);
	FD_SET(s_pingFd, &xfds);
	struct timeval tv = { 0, 0 };
	if (select(s_pingFd + 1, NULL, &wfds, &xfds, &tv) > 0) {
		// Connect attempt finished; either outcome means the network is up.
		close_rst(s_pingFd);
		s_pingFd = -1;
		s_pingState = 2;
	}
}

HttpGet* http_post_begin(const char* host, int port, const char* path,
                        const char* body, int timeout_ms)
{
	HttpGet* h = http_get_begin(host, port, path, timeout_ms);
	if (!h) return NULL;
	h->isPost = true;
	snprintf(h->body, sizeof(h->body), "%s", body ? body : "");
	return h;
}

int http_get_step(HttpGet* h)
{
	if (!h) return -1;

	if (h->st == HG_CONNECT) {
		if (osGetTime() >= h->deadline) {
			snprintf(h->stage, sizeof(h->stage), "connect timeout");
			return -1;
		}
		if (!poll_fd(h->fd, true)) return 0; // still connecting
		// A genuine refusal shows up in the exception set; SO_ERROR is
		// unreliable on the 3DS (spurious +/-26 ETXTBSY on fine sockets).
		{
			fd_set xfds;
			FD_ZERO(&xfds);
			FD_SET(h->fd, &xfds);
			struct timeval xtv = { 0, 0 };
			if (select(h->fd + 1, NULL, NULL, &xfds, &xtv) > 0) {
				snprintf(h->stage, sizeof(h->stage), "refused");
				return -1;
			}
		}
		int soerr = 0;
		socklen_t slen = sizeof(soerr);
		if (getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) == 0 && soerr != 0
		    && soerr != 26 && soerr != -26) {
			snprintf(h->stage, sizeof(h->stage), "so_error %d", soerr);
			return -1;
		}
		h->st = HG_SEND;
	}

	if (h->st == HG_SEND) {
		if (h->isPost) {
			h->reqLen = snprintf(h->req, sizeof(h->req),
			    "POST %s HTTP/1.1\r\n"
			    "Host: %s:%d\r\n"
			    "User-Agent: QuizFabric3DS/1.0\r\n"
			    "Content-Type: application/x-www-form-urlencoded\r\n"
			    "Content-Length: %d\r\n"
			    "Connection: close\r\n"
			    "\r\n"
			    "%s",
			    h->path, h->host, h->port, (int)strlen(h->body), h->body);
		} else if (h->rangeOff > 0) {
			h->reqLen = snprintf(h->req, sizeof(h->req),
			    "GET %s HTTP/1.1\r\n"
			    "Host: %s:%d\r\n"
			    "User-Agent: QuizFabric3DS/1.0\r\n"
			    "Range: bytes=%d-\r\n"
			    "Connection: close\r\n"
			    "\r\n",
			    h->path, h->host, h->port, h->rangeOff);
		} else {
			h->reqLen = snprintf(h->req, sizeof(h->req),
			    "GET %s HTTP/1.1\r\n"
			    "Host: %s:%d\r\n"
			    "User-Agent: QuizFabric3DS/1.0\r\n"
			    "Connection: close\r\n"
			    "\r\n",
			    h->path, h->host, h->port);
		}
		while (h->reqSent < h->reqLen) {
			int n = send(h->fd, h->req + h->reqSent, h->reqLen - h->reqSent, 0);
			if (n > 0) {
				h->reqSent += n;
				continue;
			}
			if (n < 0 && would_block()) {
				if (osGetTime() >= h->deadline) {
					snprintf(h->stage, sizeof(h->stage), "send timeout");
					return -1;
				}
				return 0; // try again next frame
			}
			snprintf(h->stage, sizeof(h->stage), "send error");
			return -1;
		}
		h->cap = 8192;
		h->buf = (char*)malloc(h->cap);
		if (!h->buf) {
			snprintf(h->stage, sizeof(h->stage), "oom");
			return -1;
		}
		h->st = HG_RECV;
	}

	if (h->st == HG_RECV) {
		// File mode processes at most this many bytes per call, so a fast
		// server cannot stall the render loop with endless SD writes.
		int budget = 16 * 1024;
		for (;;) {
			if (osGetTime() >= h->deadline) break;
			if (h->file) {
				if (budget <= 0) return 0;
				// File mode: stream the body straight to disk. Headers are
				// buffered in RAM only until they are complete.
				int n = recv(h->fd, h->chunk, sizeof(h->chunk), 0);
				if (n > 0) {
					budget -= n;
					h->deadline = osGetTime() + (u64)h->timeoutMs;
					if (!h->hdrsDone) {
						if (h->len + n + 1024 > h->cap) {
							int ncap = h->cap ? h->cap * 2 : 8192;
							char* nb = (char*)realloc(h->buf, ncap);
							if (!nb) {
								snprintf(h->stage, sizeof(h->stage), "hdr oom");
								break;
							}
							h->buf = nb;
							h->cap = ncap;
						}
						memcpy(h->buf + h->len, h->chunk, n);
						h->len += n;
						h->buf[h->len] = '\0';
						char* hend = strstr(h->buf, "\r\n\r\n");
						if (hend) {
							if (strncmp(h->buf, "HTTP/1.", 7) == 0) {
								h->status = atoi(h->buf + 9);
							}
							for (char* p = h->buf; p < hend; p++) {
								if (strncasecmp(p, "Content-Length:", 15) == 0) {
									h->contentLen = atoi(p + 15);
									break;
								}
							}
							h->hdrsDone = true;
							int bodyLen = h->len - (int)(hend + 4 - h->buf);
							h->len = bodyLen; // h->len = body bytes so far
							if (h->status == 404) {
								snprintf(h->stage, sizeof(h->stage), "http 404");
								break;
							}
							// A 200 despite our Range header means the server
							// ignored it: restart the file from scratch.
							if (h->rangeOff > 0 && h->status == 200) {
								fclose(h->file);
								h->file = fopen(h->fpath, "wb");
								h->offset = 0;
								if (!h->file) {
									snprintf(h->stage, sizeof(h->stage), "reopen");
									break;
								}
							}
							if (h->contentLen >= 0) h->total = h->offset + h->contentLen;
							if (bodyLen > 0) {
								fwrite(hend + 4, 1, bodyLen, h->file);
							}
							free(h->buf);
							h->buf = NULL;
							h->cap = 0;
							if (h->contentLen >= 0 && h->len >= h->contentLen) {
								h->st = HG_DONE;
								return 1;
							}
						}
					} else {
						fwrite(h->chunk, 1, n, h->file);
						h->len += n;
						if (h->contentLen >= 0 && h->len >= h->contentLen) {
							h->st = HG_DONE;
							return 1;
						}
					}
					continue;
				}
				if (n == 0) {
					// Server closed: complete when the length matches (or was
					// unknown).
					if (h->hdrsDone && (h->contentLen < 0 || h->len >= h->contentLen)) {
						h->st = HG_DONE;
						return 1;
					}
					break;
				}
				if (would_block()) return 0;
				return 0; // SOC quirk: retry next frame
			}
			if (h->len + 1024 > h->cap) {
				int ncap = h->cap * 2;
				char* nb = (char*)realloc(h->buf, ncap);
				if (!nb) {
					snprintf(h->stage, sizeof(h->stage), "body too big");
					break;
				}
				h->buf = nb;
				h->cap = ncap;
			}
			int n = recv(h->fd, h->buf + h->len, h->cap - h->len - 1, 0);
			if (n > 0) {
				h->len += n;
				h->buf[h->len] = '\0';
				// Data is flowing: the deadline is a stall timeout, not a
				// total limit, so very long downloads (big animated GIFs)
				// survive as long as chunks keep arriving.
				h->deadline = osGetTime() + (u64)h->timeoutMs;
				// Once the headers are complete, grab Content-Length for
				// progress reporting and preallocate the whole body in one
				// shot (repeated doubling reallocs would exhaust the heap
				// part-way through a multi-megabyte GIF).
				if (h->contentLen < 0) {
					char* hend = strstr(h->buf, "\r\n\r\n");
					if (hend) {
						for (char* p = h->buf; p < hend; p++) {
							if (strncasecmp(p, "Content-Length:", 15) == 0) {
								h->contentLen = atoi(p + 15);
								break;
							}
						}
						if (h->contentLen >= 0) {
							int need = h->contentLen + (int)(hend - h->buf) + 4 + 1;
							if (need > h->cap) {
								char* nb = (char*)realloc(h->buf, (size_t)need);
								if (nb) {
									h->buf = nb;
									h->cap = need;
								} else {
									snprintf(h->stage, sizeof(h->stage), "body too big");
									break;
								}
							}
						}
					}
				}
				continue;
			}
			if (n == 0) break; // server closed
			if (would_block()) {
				// No data buffered yet. Never select() for readability on
				// the 3DS (the SOC driver lies); just retry next frame, like
				// the WebSocket pump does.
				return 0;
			}
			break; // hard error; use what we have
		}
		if (h->file) {
			// File mode only reaches here on a stall/early close: the file
			// is incomplete, never report it as done (the caller resumes
			// from the partial file next attempt).
			if (!h->stage[0]) snprintf(h->stage, sizeof(h->stage), "incomplete");
			return -1;
		}
		h->st = HG_DONE;
	}

	return (h->st == HG_DONE) ? 1 : 0;
}

char* http_get_take_body(HttpGet* h)
{
	int len = 0;
	char* b = http_get_take_body_len(h, &len);
	if (!b) return NULL;
	// The text API promises a NUL-terminated string; make it one.
	char* s = (char*)malloc((size_t)len + 1);
	if (!s) {
		free(b);
		return NULL;
	}
	memcpy(s, b, (size_t)len);
	s[len] = '\0';
	free(b);
	return s;
}

char* http_get_take_body_len(HttpGet* h, int* outLen)
{
	if (!h || h->st != HG_DONE || !h->buf) return NULL;
	h->buf[h->len] = '\0';
	char* body = strstr(h->buf, "\r\n\r\n");
	if (!body) return NULL;
	body += 4;
	int n = h->len - (int)(body - h->buf);
	char* out = (char*)malloc((size_t)n);
	if (!out) return NULL;
	memcpy(out, body, (size_t)n);
	if (outLen) *outLen = n;
	return out;
}

// Like http_get_take_body_len but WITHOUT copying: ownership of the receive
// buffer transfers to the caller (free() it), and h->buf becomes NULL. Used
// for huge downloads (animated GIFs) so the body never exists twice in RAM.
char* http_get_take_body_keep(HttpGet* h, int* outLen)
{
	if (!h || h->st != HG_DONE || !h->buf) return NULL;
	char* body = strstr(h->buf, "\r\n\r\n");
	if (!body) return NULL;
	body += 4;
	int n = h->len - (int)(body - h->buf);
	// Trim the header region in place so the returned pointer is exactly
	// the body, then hand over the buffer.
	memmove(h->buf, body, (size_t)n);
	char* out = h->buf;
	h->buf = NULL;
	h->len = 0;
	h->cap = 0;
	if (outLen) *outLen = n;
	return out;
}

void http_get_close(HttpGet* h)
{
	if (!h) return;
	if (h->fd >= 0) close_rst(h->fd);
	free(h->buf);
	free(h);
}
