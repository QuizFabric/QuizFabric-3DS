// Engine.IO v4 / Socket.IO v5 framing on top of ws.c.
//
// QuizFabric serves socket.io 4.x, which speaks Engine.IO protocol 4. We skip
// the usual HTTP long-poll handshake and open the WebSocket transport directly
// (GET /socket.io/?EIO=4&transport=websocket), which the protocol allows.
#pragma once

#include "json.h"
#include "ws.h"

#include <stdbool.h>

#define SIO_PATH "/socket.io/?EIO=4&transport=websocket"

typedef enum {
	SIO_NONE = 0,
	SIO_OPEN,          // Engine.IO open; session parameters received
	SIO_PING,          // heartbeat; caller should reply with sio_pong()
	SIO_CONNECTED,     // namespace joined; emitting is now legal
	SIO_DISCONNECTED,
	SIO_CONNECT_ERROR,
	SIO_EVENT,
	SIO_UNKNOWN
} SioKind;

// Holds a parsed JsDoc, so this is a large struct (tens of KB).
// Declare it static or global, never on the stack.
typedef struct {
	SioKind kind;
	char    event[64];
	JsDoc   doc;
	int     data;        // node index of the event's first argument, or -1
	int     pingInterval; // valid after SIO_OPEN
	int     pingTimeout;  // valid after SIO_OPEN
} SioMsg;

// Decodes one Engine.IO text message. `raw` is mutated in place.
void sio_decode(char* raw, int len, SioMsg* out);

// Joins the default namespace. Send once, after SIO_OPEN.
bool sio_connect_ns(WsConn* ws);

// Replies to a SIO_PING.
bool sio_pong(WsConn* ws);

// Emits an event. `argsJson` is a single pre-serialised JSON argument
// (typically an object literal), or NULL for no arguments.
bool sio_emit(WsConn* ws, const char* event, const char* argsJson);
