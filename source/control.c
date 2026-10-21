// Control panel implementation. See control.h.

#include "control.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "sio.h"
#include "ui.h"
#include "ws.h"

#define CTRL_MAX_PLAYERS 24
#define CTRL_MAX_ANSWERS 48
#define CTRL_MAX_BOARD   24
#define CTRL_NAME_LEN    48
#define CTRL_QTEXT_LEN   640
#define CTRL_ANS_LEN     128
#define CTRL_FEED_LEN    (CTRL_NAME_LEN + CTRL_ANS_LEN + 4)

#define CTRL_ROW_VISIBLE 9
#define CTRL_ROW_H       17.0f
#define CTRL_ROW_Y0      18.0f  // first row, relative to the panel top

typedef struct {
	char name[CTRL_NAME_LEN];
	bool answered;   // submitted something for the current question
} CtrlPlayer;

typedef struct {
	char name[CTRL_NAME_LEN];
	int  score;
	int  lastCorrect; // -1 unknown, 0 wrong, 1 right
	bool left;
} CtrlBoardRow;

typedef struct {
	bool  active;
	bool  wantExit;
	char  host[80];
	int   port;
	char  gameCode[8];
	char  err[160];

	WsConn* ws;
	bool  nsSent;
	bool  ctrlSent;
	int   retries;
	u64   retryAt;

	HttpGet* hget;        // non-blocking control-code lookup
	int   phase;          // 0 resolving, 1 connecting, 2 live
	char  controlCode[8];
	int   resolveTries;

	// Live state from the server
	char  question[CTRL_QTEXT_LEN];
	bool  inQuestion;
	bool  inBoard;
	bool  inFinal;
	char  correctAnswer[CTRL_ANS_LEN];
	int   nPlayers;
	CtrlPlayer players[CTRL_MAX_PLAYERS];
	int   nFeed;
	char  feed[CTRL_MAX_ANSWERS][CTRL_FEED_LEN];
	CtrlBoardRow board[CTRL_MAX_BOARD];
	int   nBoard;

	// Local control state
	int   selPlayer;   // index into players, or -1
	bool  autoplay;
	u64   autoAt;      // when autoplay fires; 0 while idle
	char  popup[160];
	u64   popupUntil;
} Ctrl;

static Ctrl    C;
static SioMsg  c_msg;   // embeds a JsDoc; must not live on the stack
static char    c_rx[16384];

// ------------------------------------------------------------------ layout

typedef struct { float x, y, w, h; } CRect;

static CRect c_players;   // left panel
static CRect c_feed;      // right panel
static CRect c_btn[4];    // NEXT / REVEAL / AUTOPLAY / KICK

static void c_layout(void)
{
	c_players.x = 6;  c_players.y = 22; c_players.w = 150; c_players.h = 168;
	c_feed.x    = 164; c_feed.y    = 22; c_feed.w    = 150; c_feed.h    = 168;

	float bw = 71.0f, gap = 4.0f;
	for (int i = 0; i < 4; i++) {
		c_btn[i].x = 6 + i * (bw + gap);
		c_btn[i].y = 202;
		c_btn[i].w = bw;
		c_btn[i].h = 30;
	}
}

// ------------------------------------------------------------------ players

static int c_find_player(const char* name)
{
	for (int i = 0; i < C.nPlayers; i++) {
		if (!strcmp(C.players[i].name, name)) return i;
	}
	return -1;
}

static void c_add_player(const char* name)
{
	if (C.nPlayers >= CTRL_MAX_PLAYERS) return;
	if (c_find_player(name) >= 0) return;
	snprintf(C.players[C.nPlayers].name, CTRL_NAME_LEN, "%s", name);
	C.players[C.nPlayers].answered = false;
	C.nPlayers++;
}

static void c_remove_player(const char* name)
{
	int i = c_find_player(name);
	if (i < 0) return;
	for (int j = i; j + 1 < C.nPlayers; j++) C.players[j] = C.players[j + 1];
	C.nPlayers--;
	if (C.selPlayer == i) C.selPlayer = -1;
	else if (C.selPlayer > i) C.selPlayer--;
}

static void c_popup(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(C.popup, sizeof(C.popup), fmt, ap);
	va_end(ap);
	C.popupUntil = osGetTime() + 3000;
}

// ------------------------------------------------------------------ lifecycle

bool ctrl_start(const char* host, int port, const char* controlCode,
                char* err, int errSz)
{
	(void)err;
	(void)errSz;
	ctrl_exit();
	memset(&C, 0, sizeof(C));

	snprintf(C.host, sizeof(C.host), "%s", host);
	C.port = port;
	snprintf(C.controlCode, sizeof(C.controlCode), "%s", controlCode);
	C.selPlayer = -1;
	C.phase = 0; // resolve the code first, frame by frame in ctrl_step()
	C.active = true;
	return true;
}

bool ctrl_is_active(void) { return C.active; }
bool ctrl_wants_exit(void) { return C.wantExit; }
const char* ctrl_error(void) { return C.err; }

void ctrl_exit(void)
{
	if (C.ws) {
		ws_close(C.ws);
		C.ws = NULL;
	}
	C.active = false;
	C.wantExit = false;
}

// ------------------------------------------------------------------ events

static void c_emit(const char* event)
{
	if (!C.ws || !C.ctrlSent) return;
	char args[64];
	snprintf(args, sizeof(args), "{\"code\":\"%s\"}", C.gameCode);
	sio_emit(C.ws, event, args);
}

static void c_read_board(SioMsg* m)
{
	int arr = json_obj_get(&m->doc, m->data, "leaderboard");
	int n   = json_arr_len(&m->doc, arr);
	if (n > CTRL_MAX_BOARD) n = CTRL_MAX_BOARD;
	C.nBoard = n;
	for (int i = 0; i < n; i++) {
		int row = json_arr_at(&m->doc, arr, i);
		snprintf(C.board[i].name, CTRL_NAME_LEN, "%s",
		         json_ostr(&m->doc, row, "name", "?"));
		C.board[i].score = (int)json_onum(&m->doc, row, "score", 0);
		C.board[i].left  = json_obool(&m->doc, row, "left", false);

		int lc = json_obj_get(&m->doc, row, "last_correct");
		if (lc < 0 || m->doc.nodes[lc].type == JS_NULL) C.board[i].lastCorrect = -1;
		else C.board[i].lastCorrect = json_bool(&m->doc, lc, false) ? 1 : 0;
	}
}

static void c_on_event(SioMsg* m)
{
	const char* ev = m->event;

	if (!strcmp(ev, "player_joined")) {
		c_add_player(json_ostr(&m->doc, m->data, "name", "Player"));
		return;
	}
	if (!strcmp(ev, "player_left")) {
		c_remove_player(json_ostr(&m->doc, m->data, "name", ""));
		return;
	}
	if (!strcmp(ev, "answer_received")) {
		const char* name = json_ostr(&m->doc, m->data, "name", "Someone");
		const char* ans  = json_ostr(&m->doc, m->data, "answer", "?");
		int i = c_find_player(name);
		if (i >= 0) C.players[i].answered = true;
		if (C.nFeed < CTRL_MAX_ANSWERS) {
			snprintf(C.feed[C.nFeed], CTRL_FEED_LEN, "%s: %s", name, ans);
			C.nFeed++;
		}
		return;
	}
	if (!strcmp(ev, "new_question")) {
		snprintf(C.question, sizeof(C.question), "%s",
		         json_ostr(&m->doc, m->data, "question", ""));
		C.inQuestion = true;
		C.inBoard = false;
		C.inFinal = false;
		C.correctAnswer[0] = '\0';
		C.nFeed = 0;
		for (int i = 0; i < C.nPlayers; i++) C.players[i].answered = false;
		C.autoAt = 0;
		return;
	}
	if (!strcmp(ev, "answers_revealed")) {
		snprintf(C.correctAnswer, sizeof(C.correctAnswer), "%s",
		         json_ostr(&m->doc, m->data, "correct_answer", ""));
		return;
	}
	if (!strcmp(ev, "leaderboard")) {
		snprintf(C.correctAnswer, sizeof(C.correctAnswer), "%s",
		         json_ostr(&m->doc, m->data, "correct_answer", ""));
		c_read_board(m);
		C.inQuestion = false;
		C.inBoard = true;
		C.inFinal = false;
		C.nFeed = 0;
		if (C.autoplay) C.autoAt = osGetTime() + 5000;
		return;
	}
	if (!strcmp(ev, "final_leaderboard")) {
		c_read_board(m);
		C.inQuestion = false;
		C.inBoard = false;
		C.inFinal = true;
		C.autoAt = 0;
		return;
	}
	if (!strcmp(ev, "show_popup")) {
		c_popup("%s", json_ostr(&m->doc, m->data, "message", ""));
		return;
	}
	if (!strcmp(ev, "error")) {
		snprintf(C.err, sizeof(C.err), "%s",
		         json_ostr(&m->doc, m->data, "message", "Control error"));
		C.wantExit = true;
		return;
	}
}

void ctrl_step(void)
{
	if (!C.active) return;

	// Phase 0: resolve the control code with a non-blocking HTTP GET so the
	// screen can keep drawing progress while the 3DS SOC stack warms up.
	if (C.phase == 0) {
		if (!C.hget) {
			char path[96];
			snprintf(path, sizeof(path), "/resolve_control/%s", C.controlCode);
			C.hget = http_get_begin(C.host, C.port, path, 4000);
			if (!C.hget) {
				C.resolveTries++;
				c_popup("Connection refused - retrying (%d)", C.resolveTries);
				return; // a fresh attempt starts next frame
			}
			return;
		}
		int r = http_get_step(C.hget);
		if (r == 0) return;
		if (r < 0) {
			http_get_close(C.hget);
			C.hget = NULL;
			C.resolveTries++;
			c_popup("Connection refused - retrying (%d)", C.resolveTries);
			return;
		}
		// Got a response. Parse the game code out of it.
		char* body = http_get_take_body(C.hget);
		http_get_close(C.hget);
		C.hget = NULL;
		bool ok = false;
		if (body) {
			static JsDoc doc; // large; keep out of the stack
			if (json_parse(&doc, body)) {
				const char* gc = json_ostr(&doc, 0, "game_code", "");
				if (gc[0]) {
					snprintf(C.gameCode, sizeof(C.gameCode), "%s", gc);
					ok = true;
				}
			}
			free(body);
		}
		if (!ok) {
			snprintf(C.err, sizeof(C.err), "Invalid control code");
			C.wantExit = true;
			return;
		}
		C.phase = 1;
		C.ws = ws_begin(C.host, C.port, SIO_PATH);
		if (!C.ws) {
			snprintf(C.err, sizeof(C.err), "Out of memory");
			C.wantExit = true;
		}
		return;
	}

	if (!C.ws) {
		// Retry timer: reconnect as soon as the (zero-length) gap is up.
		if (C.retryAt && osGetTime() >= C.retryAt) {
			C.retryAt = 0;
			C.ws = ws_begin(C.host, C.port, SIO_PATH);
			if (!C.ws) {
				snprintf(C.err, sizeof(C.err), "Out of memory");
				C.wantExit = true;
			}
		}
		return;
	}

	WsState st = ws_step(C.ws);
	if (st == WSS_ERR) {
		C.retries++;
		C.retryAt = osGetTime();
		ws_close(C.ws);
		C.ws = NULL;
		C.nsSent = false;
		C.ctrlSent = false;
		c_popup("Connection refused - retrying (%d)", C.retries);
		return;
	}
	if (st == WSS_CLOSED) {
		snprintf(C.err, sizeof(C.err), "Disconnected from server");
		C.wantExit = true;
		return;
	}
	if (st != WSS_OPEN) return;

	if (C.retries) {
		C.retries = 0;
		C.retryAt = 0;
	}

	int len = 0;
	int r;
	while ((r = ws_poll(C.ws, c_rx, sizeof(c_rx), &len)) == 1) {
		sio_decode(c_rx, len, &c_msg);
		switch (c_msg.kind) {
		case SIO_OPEN:
			if (!C.nsSent) {
				sio_connect_ns(C.ws);
				C.nsSent = true;
			}
			break;
		case SIO_PING:
			sio_pong(C.ws);
			break;
		case SIO_CONNECTED:
			if (!C.ctrlSent) {
				char args[64];
				snprintf(args, sizeof(args), "{\"code\":\"%s\"}", C.gameCode);
				sio_emit(C.ws, "control_join", args);
				C.ctrlSent = true;
				C.phase = 2; // live
			}
			break;
		case SIO_CONNECT_ERROR:
			snprintf(C.err, sizeof(C.err), "Server refused the controller");
			C.wantExit = true;
			return;
		case SIO_DISCONNECTED:
			snprintf(C.err, sizeof(C.err), "Server closed the session");
			C.wantExit = true;
			return;
		case SIO_EVENT:
			c_on_event(&c_msg);
			break;
		default:
			break;
		}
	}
	if (r < 0 && C.ws) {
		snprintf(C.err, sizeof(C.err), "%s", ws_error(C.ws));
		C.wantExit = true;
	}

	// Autoplay: fire the next question once the countdown runs out.
	if (C.autoAt && osGetTime() >= C.autoAt) {
		C.autoAt = 0;
		c_emit("start_round");
	}
}

// ------------------------------------------------------------------ input

static void c_kick_selected(void)
{
	if (C.selPlayer < 0 || C.selPlayer >= C.nPlayers) {
		c_popup("Tap a player to select them first");
		return;
	}
	char nameEsc[CTRL_NAME_LEN * 2];
	json_escape(C.players[C.selPlayer].name, nameEsc, sizeof(nameEsc));
	char args[192];
	snprintf(args, sizeof(args), "{\"code\":\"%s\",\"name\":\"%s\"}",
	         C.gameCode, nameEsc);
	sio_emit(C.ws, "kick_player", args);
	c_popup("Kicked %s", C.players[C.selPlayer].name);
	C.selPlayer = -1;
}

void ctrl_input(u32 kDown, touchPosition* touch, bool touched)
{
	if (!C.active) return;
	c_layout();

	if (kDown & KEY_B) {
		C.wantExit = true;
		return;
	}

	int tx = touch->px, ty = touch->py;

	// Tap a player row to select them for kicking.
	if (!C.inBoard && !C.inFinal && touched &&
	    ui_hit(c_players.x, c_players.y, c_players.w, c_players.h, tx, ty)) {
		int idx = (int)((ty - c_players.y - CTRL_ROW_Y0) / CTRL_ROW_H);
		if (idx >= 0 && idx < C.nPlayers) C.selPlayer = idx;
	}

	for (int i = 0; i < 4; i++) {
		if (!touched) continue;
		if (!ui_hit(c_btn[i].x, c_btn[i].y, c_btn[i].w, c_btn[i].h, tx, ty)) continue;
		switch (i) {
		case 0: // NEXT / START
			if (C.ctrlSent) {
				c_emit("start_round");
				C.autoAt = 0;
			}
			break;
		case 1: // REVEAL
			if (C.ctrlSent && C.inQuestion) c_emit("reveal_answers");
			break;
		case 2: // AUTOPLAY
			C.autoplay = !C.autoplay;
			c_popup(C.autoplay ? "Autoplay ON" : "Autoplay OFF");
			if (!C.autoplay) C.autoAt = 0;
			break;
		case 3: // KICK
			if (C.ctrlSent) c_kick_selected();
			break;
		}
	}
}

// ------------------------------------------------------------------ drawing

void ctrl_draw_top(void)
{
	char lines[UI_MAX_LINES][UI_LINE_MAX];
	char buf[160];

	snprintf(buf, sizeof(buf), "CONTROL  PIN %s", C.gameCode[0] ? C.gameCode : "-");
	ui_panel(0, 0, TOP_W, 26, RGBA(40, 8, 60, 80));
	ui_text(buf, 10, 4, 0.52f, CLR_SOFT);
	ui_text_right(C.autoplay ? "AUTOPLAY ON" : "AUTOPLAY OFF",
	              TOP_W - 10, 4, 0.52f, C.autoplay ? CLR_GOLD : CLR_DIM);

	if (C.phase == 0) {
		ui_text_center_shadow("CONTROL PANEL", TOP_W / 2, 72, 0.9f, CLR_WHITE);
		ui_text_center("Resolving control code...", TOP_W / 2, 120, 0.52f, CLR_SOFT);
		float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
		ui_bar(120, 152, TOP_W - 240, 6, pct, CLR_WHITE, RGBA(255, 255, 255, 60));
		snprintf(buf, sizeof(buf), "attempt %d", C.resolveTries + 1);
		ui_text_center(buf, TOP_W / 2, 172, 0.45f, CLR_DIM);
	} else if (C.phase == 1) {
		ui_text_center_shadow("CONNECTING", TOP_W / 2, 90, 0.9f, CLR_WHITE);
		ui_text_center("Talking to the server...", TOP_W / 2, 128, 0.5f, CLR_SOFT);
		float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
		ui_bar(120, 160, TOP_W - 240, 6, pct, CLR_WHITE, RGBA(255, 255, 255, 60));
	} else if (C.inQuestion) {
		float scale = 0.8f;
		int n = ui_wrap_fit(C.question, TOP_W - 48, 128, 0.85f, 0.42f,
		                    lines, UI_MAX_LINES, &scale);
		float lh = ui_line_height(scale);
		float y = 38 + (140 - n * lh) / 2;
		for (int i = 0; i < n; i++) {
			ui_text_center_shadow(lines[i], TOP_W / 2, y + i * lh, scale, CLR_WHITE);
		}

		if (C.correctAnswer[0]) {
			snprintf(buf, sizeof(buf), "ANSWER: %s", C.correctAnswer);
			ui_text_center(buf, TOP_W / 2, 198, 0.5f, CLR_GOLD);
		} else {
			int done = 0;
			for (int i = 0; i < C.nPlayers; i++) {
				if (C.players[i].answered) done++;
			}
			snprintf(buf, sizeof(buf), "%d / %d answered", done, C.nPlayers);
			ui_text_center(buf, TOP_W / 2, 198, 0.5f, CLR_SOFT);
		}
	} else if (C.inBoard) {
		ui_text_center_shadow("STANDINGS", TOP_W / 2, 58, 0.8f, CLR_WHITE);
		if (C.correctAnswer[0]) {
			snprintf(buf, sizeof(buf), "Answer: %s", C.correctAnswer);
			ui_text_center(buf, TOP_W / 2, 100, 0.45f, CLR_GOLD);
		}
		if (C.autoAt) {
			int secs = (int)((C.autoAt - osGetTime() + 999) / 1000);
			snprintf(buf, sizeof(buf), "Autoplay: next question in %ds", secs);
			ui_text_center(buf, TOP_W / 2, 142, 0.5f, CLR_SOFT);
		} else {
			ui_text_center("Tap NEXT below when ready", TOP_W / 2, 142, 0.45f, CLR_DIM);
		}
	} else if (C.inFinal) {
		ui_text_center_shadow("GAME OVER", TOP_W / 2, 74, 0.9f, CLR_WHITE);
		if (C.nBoard > 0) {
			ui_text_center("WINNER", TOP_W / 2, 120, 0.45f, CLR_DIM);
			ui_text_center_shadow(C.board[0].name, TOP_W / 2, 140, 0.8f, CLR_GOLD);
			snprintf(buf, sizeof(buf), "%d points", C.board[0].score);
			ui_text_center(buf, TOP_W / 2, 176, 0.5f, CLR_WHITE);
		}
	} else {
		ui_text_center_shadow("CONTROL PANEL", TOP_W / 2, 66, 0.9f, CLR_WHITE);
		ui_text_center("Waiting for the host to start", TOP_W / 2, 112, 0.52f, CLR_SOFT);
		snprintf(buf, sizeof(buf), "%d players in the game", C.nPlayers);
		ui_text_center(buf, TOP_W / 2, 148, 0.5f, CLR_GOLD);
		ui_text_center("Tap START below to begin", TOP_W / 2, 192, 0.45f, CLR_DIM);
	}
}

void ctrl_draw_bottom(void)
{
	char buf[200];

	c_layout();
	ui_text_right("B = exit", BOT_W - 10, 8, 0.36f, CLR_DIM);

	if (C.phase < 2) {
		// Still resolving the code or connecting; no panels yet.
		ui_text_center(C.phase == 0 ? "Looking up the control code..."
		                            : "Connecting to the server...",
		               BOT_W / 2, 110, 0.5f, CLR_SOFT);
		ui_text_center("Press B to cancel", BOT_W / 2, 140, 0.42f, CLR_DIM);
	} else if (C.inBoard || C.inFinal) {
		ui_text(C.inFinal ? "FINAL STANDINGS" : "STANDINGS", 10, 8, 0.5f, CLR_SOFT);
		const float rowH = 26.0f;
		for (int v = 0; v < 6; v++) {
			if (v >= C.nBoard) break;
			float y = 24 + v * (rowH + 2);
			ui_panel_hi(8, y, BOT_W - 16, rowH, CLR_GLASS);
			snprintf(buf, sizeof(buf), "%d", v + 1);
			ui_text(buf, 16, y + 5, 0.48f, v == 0 ? CLR_GOLD : CLR_SOFT);
			snprintf(buf, sizeof(buf), "%s%s", C.board[v].name,
			         C.board[v].left ? " (left)" : "");
			ui_text(buf, 42, y + 5, 0.46f, C.board[v].left ? CLR_DIM : CLR_WHITE);
			snprintf(buf, sizeof(buf), "%d", C.board[v].score);
			ui_text_right(buf, BOT_W - 16, y + 5, 0.48f, CLR_WHITE);
		}
		if (C.nBoard == 0) {
			ui_text_center("No scores yet", BOT_W / 2, 110, 0.5f, CLR_DIM);
		}
	} else {
		// Players (left) and live answers (right).
		ui_panel_hi(c_players.x, c_players.y, c_players.w, c_players.h, CLR_GLASS);
		ui_text("PLAYERS", c_players.x + 8, c_players.y + 5, 0.4f, CLR_DIM);
		for (int i = 0; i < C.nPlayers && i < CTRL_ROW_VISIBLE; i++) {
			float y = c_players.y + CTRL_ROW_Y0 + i * CTRL_ROW_H;
			bool sel = (i == C.selPlayer);
			if (sel) {
				C2D_DrawRectSolid(c_players.x + 2, y - 1, 0.08f,
				                  c_players.w - 4, CTRL_ROW_H,
				                  ui_fade(CLR_GOLD, 0.25f));
			}
			ui_text(C.players[i].name, c_players.x + 10, y + 2, 0.44f,
			        sel ? CLR_GOLD : CLR_WHITE);
			if (C.players[i].answered) {
				ui_text_right("OK", c_players.x + c_players.w - 8, y + 2,
				              0.44f, CLR_GOOD);
			}
		}
		if (C.nPlayers == 0) {
			ui_text_center("nobody yet", c_players.x + c_players.w / 2,
			               c_players.y + 84, 0.45f, CLR_DIM);
		}

		ui_panel_hi(c_feed.x, c_feed.y, c_feed.w, c_feed.h, CLR_GLASS);
		ui_text("ANSWERS", c_feed.x + 8, c_feed.y + 5, 0.4f, CLR_DIM);
		int start = C.nFeed > CTRL_ROW_VISIBLE ? C.nFeed - CTRL_ROW_VISIBLE : 0;
		int shown = 0;
		for (int i = start; i < C.nFeed; i++) {
			float y = c_feed.y + CTRL_ROW_Y0 + shown * CTRL_ROW_H;
			ui_text(C.feed[i], c_feed.x + 8, y + 2, 0.42f, CLR_SOFT);
			shown++;
		}
		if (C.nFeed == 0) {
			ui_text_center("answers appear here", c_feed.x + c_feed.w / 2,
			               c_feed.y + 84, 0.42f, CLR_DIM);
		}
	}

	// Button row: NEXT / REVEAL / AUTOPLAY / KICK.
	const char* l0 = (C.inQuestion || C.inBoard) ? "NEXT" : "START";
	static const char* labels[4] = { "?", "REVEAL", "AUTO", "KICK" };
	for (int i = 0; i < 4; i++) {
		u32 fill = CLR_GLASS;
		if (i == 0) fill = RGB(56, 214, 160);
		if (i == 1 && C.inQuestion) fill = RGB(255, 208, 92);
		if (i == 2 && C.autoplay) fill = CLR_GLASS_HI;
		if (i == 3 && C.selPlayer >= 0) fill = RGB(255, 92, 116);
		ui_tile(c_btn[i].x, c_btn[i].y, c_btn[i].w, c_btn[i].h, fill, false);
		const char* lbl = (i == 0) ? l0 : labels[i];
		ui_text_center(lbl, c_btn[i].x + c_btn[i].w / 2, c_btn[i].y + 7,
		               0.48f, i == 0 ? CLR_INK : CLR_WHITE);
	}

	// Popup bar (kick confirmations, toggles) above the button row.
	if (C.popup[0] && osGetTime() < C.popupUntil) {
		float w = ui_text_width(C.popup, 0.45f) + 24;
		if (w > BOT_W - 20) w = BOT_W - 20;
		ui_panel(BOT_W / 2 - w / 2, 174, w, 20, RGBA(30, 6, 50, 210));
		ui_text_center(C.popup, BOT_W / 2, 177, 0.45f, CLR_WHITE);
	}
}
