#include "sio.h"

#include <stdio.h>
#include <string.h>

// Engine.IO packet types: 0 open, 1 close, 2 ping, 3 pong, 4 message,
// 5 upgrade, 6 noop. Socket.IO types (the byte after a '4'): 0 CONNECT,
// 1 DISCONNECT, 2 EVENT, 3 ACK, 4 CONNECT_ERROR, 5/6 binary variants.

void sio_decode(char* raw, int len, SioMsg* out)
{
	out->kind  = SIO_NONE;
	out->data  = -1;
	out->event[0] = '\0';
	out->doc.ok   = false;

	if (!raw || len <= 0) return;

	switch (raw[0]) {
	case '0': { // Engine.IO OPEN
		out->kind = SIO_OPEN;
		out->pingInterval = 25000;
		out->pingTimeout  = 20000;
		if (len > 1 && json_parse(&out->doc, raw + 1)) {
			out->pingInterval = (int)json_onum(&out->doc, 0, "pingInterval", 25000);
			out->pingTimeout  = (int)json_onum(&out->doc, 0, "pingTimeout", 20000);
		}
		return;
	}
	case '1':
		out->kind = SIO_DISCONNECTED;
		return;
	case '2':
		out->kind = SIO_PING;
		return;
	case '3': // pong (we never ping first, so nothing to do)
		out->kind = SIO_NONE;
		return;
	case '4':
		break; // Socket.IO message; fall through to the inner parse
	default:
		out->kind = SIO_UNKNOWN;
		return;
	}

	if (len < 2) {
		out->kind = SIO_UNKNOWN;
		return;
	}

	char sioType = raw[1];
	char* p = raw + 2;

	// Optional namespace, e.g. 42/admin,["ev",{}]. We only use the default
	// namespace, but skip it defensively so a stray one cannot desync us.
	if (*p == '/') {
		char* comma = strchr(p, ',');
		if (!comma) {
			out->kind = SIO_UNKNOWN;
			return;
		}
		p = comma + 1;
	}
	// Optional ack id.
	while (*p >= '0' && *p <= '9') p++;

	switch (sioType) {
	case '0':
		out->kind = SIO_CONNECTED;
		return;
	case '1':
		out->kind = SIO_DISCONNECTED;
		return;
	case '4':
		out->kind = SIO_CONNECT_ERROR;
		if (*p && json_parse(&out->doc, p)) out->data = 0;
		return;
	case '2':
		break; // EVENT
	default:
		out->kind = SIO_UNKNOWN;
		return;
	}

	// EVENT payload is ["name", arg0, arg1, ...].
	if (!json_parse(&out->doc, p)) {
		out->kind = SIO_UNKNOWN;
		return;
	}
	int nameNode = json_arr_at(&out->doc, 0, 0);
	const char* name = json_str(&out->doc, nameNode, NULL);
	if (!name) {
		out->kind = SIO_UNKNOWN;
		return;
	}
	snprintf(out->event, sizeof(out->event), "%s", name);
	out->data = json_arr_at(&out->doc, 0, 1); // -1 when the event has no args
	out->kind = SIO_EVENT;
}

bool sio_connect_ns(WsConn* ws)
{
	return ws_send_text(ws, "40", 2);
}

bool sio_pong(WsConn* ws)
{
	return ws_send_text(ws, "3", 1);
}

bool sio_emit(WsConn* ws, const char* event, const char* argsJson)
{
	char buf[2048];
	int n;
	if (argsJson && *argsJson) {
		n = snprintf(buf, sizeof(buf), "42[\"%s\",%s]", event, argsJson);
	} else {
		n = snprintf(buf, sizeof(buf), "42[\"%s\"]", event);
	}
	if (n < 0 || n >= (int)sizeof(buf)) return false;
	return ws_send_text(ws, buf, n);
}
