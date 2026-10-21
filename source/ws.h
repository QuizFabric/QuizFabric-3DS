// Non-blocking TCP + WebSocket (RFC 6455) client for the 3DS.
//
// Connection setup is a per-frame state machine (ws_step) rather than a
// blocking call, so the render loop never stalls on a slow or wrong address.
#pragma once

#include <stdbool.h>
#include <stdio.h>

typedef enum {
	WSS_RESOLVE = 0, // looking up the host
	WSS_CONNECT,     // TCP handshake in flight
	WSS_SEND_REQ,    // sending the HTTP Upgrade request
	WSS_HDRS,        // reading the 101 response
	WSS_OPEN,        // ready for ws_send_text / ws_poll
	WSS_CLOSED,      // peer closed cleanly
	WSS_ERR          // failed; see ws_error()
} WsState;

typedef struct WsConn WsConn;

// Brings up the SOC service. Call once at startup.
bool net_init(void);
void net_exit(void);

// Starts a connection. Never blocks on the TCP handshake. Returns NULL only
// if allocation failed. Drive it with ws_step() until WSS_OPEN or WSS_ERR.
WsConn* ws_begin(const char* host, int port, const char* path);

// Advances the handshake and pumps the receive buffer. Call once per frame.
WsState ws_step(WsConn* c);
WsState ws_state(WsConn* c);
const char* ws_error(WsConn* c);

// Sends one text frame (masked, as clients must).
bool ws_send_text(WsConn* c, const char* data, int len);

// Pops one complete text message into out.
// Returns 1 on message, 0 if none pending, -1 if the connection is gone.
// Control frames (ping/close) are handled internally and never surface here.
int ws_poll(WsConn* c, char* out, int outsz, int* outlen);

void ws_close(WsConn* c);

// Blocking HTTP/1.1 GET. Returns a malloc'd, NUL-terminated body, or NULL.
// Used only for the one-shot avatar list; the quiz itself runs over WebSocket.
char* http_get(const char* host, int port, const char* path, int timeout_ms);

// Non-blocking HTTP GET driven per frame. Same request as http_get(), but the
// caller advances it with http_get_step() so the render loop keeps drawing
// progress (e.g. the control-code lookup).
typedef struct HttpGet HttpGet;

HttpGet* http_get_begin(const char* host, int port, const char* path, int timeout_ms);

// Streams the GET body straight to `file`, resuming at `offset` bytes via a
// Range header (the server may restart from 0 instead; the file is reopened
// truncated in that case - `fpath` is its path). `file` must already be
// open for append. Drive with http_get_step; the file is NOT closed by
// http_get_close.
HttpGet* http_get_begin_file(const char* host, int port, const char* path,
                             int timeout_ms, FILE* file, int offset,
                             const char* fpath);

// Same as http_get_begin but POSTs a small urlencoded form body
// (Content-Type: application/x-www-form-urlencoded).
HttpGet* http_post_begin(const char* host, int port, const char* path,
                        const char* body, int timeout_ms);

int      http_get_step(HttpGet* h);       // 0 busy, 1 done, -1 failed

int      http_get_status(HttpGet* h);     // HTTP status code, -1 before headers
int      http_get_downloaded(HttpGet* h); // body bytes received (file mode:
                                          // includes the resume base)

// Download progress: 0..1 when the response has a Content-Length, -1 while
// still connecting or when the length is unknown, 1 when done.
float    http_get_progress(HttpGet* h);

char*    http_get_take_body(HttpGet* h);  // malloc'd body after step()==1; free() it

// Binary-safe variant (the body can contain NULs): returns malloc'd bytes and
// their length, or NULL.
char*    http_get_take_body_len(HttpGet* h, int* outLen);

// Same, but WITHOUT copying: ownership of the receive buffer transfers to
// the caller (free() it once). For huge bodies that must not exist twice in
// RAM (animated GIFs).
char*    http_get_take_body_keep(HttpGet* h, int* outLen);

void     http_get_close(HttpGet* h);

// Diagnostics: why the last http_get_begin() returned NULL, and which stage
// failed when http_get_step() returned -1.
const char* http_begin_error(void);
const char* http_get_stage(HttpGet* h);

// ------------------------------------------------------------- connectivity
// Reachability check for offline mode: a non-blocking TCP connect to
// 8.8.8.8:53. Any response proves the network is up; 15s of silence means
// no internet. net_ping_state(): 0 idle, 1 checking, 2 online, 3 offline.
int  net_ping_state(void);
void net_ping_start(void);
void net_ping_step(void);
