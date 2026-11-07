#include "net.h"

#include <nds.h>
#include <dswifi9.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#define NET_UA "QuizFabricDS/1.0"

static char s_ip[16] = "0.0.0.0";

// Frame-counting clock: one vblank wait = one frame ~= 16ms. Good enough
// for download timeouts and point decay, and it keeps the app entirely
// IRQ-free (see net_wait_vblank).
static unsigned s_ms;

unsigned net_ticks_ms(void)
{
	return s_ms;
}

void net_wait_vblank(void)
{
	while (REG_VCOUNT >= 192);
	while (REG_VCOUNT < 192);
	s_ms += 16;
}

void net_init(void)
{
	Wifi_InitDefault(true);
	Wifi_AutoConnect();
}

int net_wifi_state(void)
{
	int st = Wifi_AssocStatus();
	if (st == ASSOCSTATUS_ASSOCIATED) {
		struct in_addr gw, mask, dns1, dns2;
		struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
		snprintf(s_ip, sizeof(s_ip), "%s", inet_ntoa(ip));
		return 1;
	}
	if (st == ASSOCSTATUS_CANNOTCONNECT) return -1;
	return 0;
}

void net_disconnect(void)
{
	Wifi_DisconnectAP();
}

const char* net_ip(void)
{
	return s_ip;
}

int net_urlencode(const char* in, char* out, int outsz)
{
	int o = 0;
	if (outsz <= 0) return 0;
	for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
		if (o + 4 >= outsz) break;
		if (*p == ' ') {
			out[o++] = '%';
			out[o++] = '2';
			out[o++] = '0';
		} else {
			out[o++] = (char)*p;
		}
	}
	out[o] = '\0';
	return o;
}

// Parses the header block for the content length; -1 when absent.
static int parse_content_length(const char* hdr)
{
	for (const char* p = hdr; *p; p++) {
		if ((p[0] == 'C' || p[0] == 'c') &&
		    (p[1] == 'o' || p[1] == 'O') &&
		    (p[2] == 'n' || p[2] == 'N') &&
		    (p[3] == 't' || p[3] == 'T') &&
		    (p[4] == 'e' || p[4] == 'E') &&
		    (p[5] == 'n' || p[5] == 'N') &&
		    (p[6] == 't' || p[6] == 'T') &&
		    (p[7] == '-') && (p[8] == 'L' || p[8] == 'l')) {
			return atoi(p + 15); // "content-length:" is 15 chars
		}
	}
	return -1;
}

int net_http_get(const char* host, int port, const char* path,
                 char* out, int outsz, int timeoutMs,
                 NetProgress progress, void* user)
{
	if (!out || outsz <= 0) return -1;

	struct hostent* he = gethostbyname(host);
	if (!he || !he->h_addr_list || !he->h_addr_list[0]) return -1;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port   = htons((u16)port);
	memcpy(&addr.sin_addr, he->h_addr_list[0], 4);

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
		closesocket(fd);
		return -1;
	}

	// Non-blocking: dswifi has no select(), so we poll FIONREAD instead.
	int one = 1;
	ioctl(fd, FIONBIO, &one);

	char req[512];
	int rl = snprintf(req, sizeof(req),
	                  "GET %s HTTP/1.1\r\n"
	                  "Host: %s\r\n"
	                  "User-Agent: " NET_UA "\r\n"
	                  "Connection: close\r\n"
	                  "\r\n",
	                  path, host);
	send(fd, req, rl, 0);

	static char hdr[2048]; // static: too big for the 16KB DS stack
	int  hdrLen     = 0;
	bool inBody     = false;
	int  contentLen = -1;
	int  outLen     = 0;
	int  cancelled  = 0;

	unsigned lastData = net_ticks_ms();
	unsigned deadline = lastData + (unsigned)timeoutMs;

	while (1) {
		int avail = 0;
		if (ioctl(fd, FIONREAD, &avail) != 0) break;
		if (avail > 0) {
			char tmp[512];
			int want = avail > (int)sizeof(tmp) ? (int)sizeof(tmp) : avail;
			int n = recv(fd, tmp, want, 0);
			if (n <= 0) break; // peer closed or error
			lastData = net_ticks_ms();

			int i = 0;
			while (i < n) {
				if (!inBody) {
					if (hdrLen < (int)sizeof(hdr) - 1) hdr[hdrLen++] = tmp[i];
					i++;
					hdr[hdrLen] = '\0';
					if (hdrLen >= 4 && !strncmp(hdr + hdrLen - 4, "\r\n\r\n", 4)) {
						hdr[hdrLen - 4] = '\0';
						contentLen = parse_content_length(hdr);
						inBody = true;
					}
				} else {
					if (outLen < outsz - 1) out[outLen++] = tmp[i];
					i++;
				}
			}
			if (progress && inBody) {
				if (!progress(user, outLen, contentLen)) {
					cancelled = 1;
					break;
				}
			}
		}
		if (inBody) {
			if (contentLen >= 0 && outLen >= contentLen) break;
		}
		if (net_ticks_ms() - lastData > (unsigned)timeoutMs) break; // stall
		if (net_ticks_ms() > deadline) break;
		swiWaitForVBlank();
	}

	closesocket(fd);
	out[outLen] = '\0';

	if (cancelled) return -2;
	if (!inBody) return -1;
	return outLen;
}
