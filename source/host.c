// Hosting from the 3DS. See host.h.

#define _GNU_SOURCE 1 // strcasestr in newlib

#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "json.h"
#include "precache.h"
#include "thumb.h"
#include "ui.h"
#include "ws.h"

#define HOST_MAX_QUIZZES 128
#define HOST_NAME_LEN    96

// Quiz grid: 2 tiles per row, 3 rows visible.
#define HOST_COLS     2
#define HOST_ROWS     3
#define HOST_TILE_W   145.0f
#define HOST_TILE_H   58.0f
#define HOST_GAPX     10.0f
#define HOST_GAPY     8.0f

// Phases: 0 fetching list, 1 browsing, 2 hosting, 3 showing the codes.
typedef struct {
	char name[HOST_NAME_LEN];
	bool banner; // server has a banner image for this quiz
} HostQuiz;

typedef struct {
	bool  active;
	bool  wantExit;
	bool  wantControl;
	int   phase;
	char  host[80];
	int   port;
	int   nQuizzes;
	HostQuiz quizzes[HOST_MAX_QUIZZES];
	int   sel;        // selected quiz position while browsing
	int   scroll;
	bool  heldNav;    // Up/Down held: becomes fast scroll after 2s
	u64   holdAt;
	bool  fast;
	u64   nextRepeat;
	int   cpDir;      // circle pad direction (1 l, 2 r, 3 up, 4 down)
	u64   cpRepeatAt; // circle pad auto-repeat
	bool  comboArm;   // X+SELECT armed (SELECT must release first)
	char  search[96]; // Y search filter (empty = no filter)
	int   fidx[HOST_MAX_QUIZZES]; // filtered positions -> real quiz index
	int   nFidx;
	char  gameCode[8];
	char  ctrlCode[8];
	HttpGet* hget;
	char  fetchPath[256];
	char  buf[16384]; // /api/quizzes now carries banner flags per quiz
} Host;

static Host H;

// ------------------------------------------------------------------ helpers

static void url_encode(char* dst, int dstsz, const char* src)
{
	char* d = dst;
	for (const char* s = src; *s && d < dst + dstsz - 4; s++) {
		if (*s == ' ') {
			*d++ = '%'; *d++ = '2'; *d++ = '0';
		} else {
			*d++ = *s;
		}
	}
	*d = '\0';
}

static void fetch_begin(const char* path)
{
	if (H.hget) {
		http_get_close(H.hget);
		H.hget = NULL;
	}
	snprintf(H.fetchPath, sizeof(H.fetchPath), "%s", path);
	H.hget = http_get_begin(H.host, H.port, H.fetchPath, 8000);
}

// 1 done (body in H.buf), 0 busy, -1 failed.
static int fetch_poll(void)
{
	if (!H.hget) return -1;
	int r = http_get_step(H.hget);
	if (r == 0) return 0;
	int ok = 0;
	if (r == 1) {
		char* body = http_get_take_body(H.hget);
		if (body) {
			snprintf(H.buf, sizeof(H.buf), "%s", body);
			free(body);
			ok = 1;
		}
	}
	http_get_close(H.hget);
	H.hget = NULL;
	return ok ? 1 : -1;
}

// ------------------------------------------------------------------ lifecycle

void host_start(const char* host, int port)
{
	host_exit();
	memset(&H, 0, sizeof(H));
	snprintf(H.host, sizeof(H.host), "%s", host);
	H.port = port;
	H.phase = 0;
	H.active = true;
	thumb_begin(H.host, H.port);
	fetch_begin("/api/quizzes");
}

bool host_wants_exit(void) { return H.wantExit; }
bool host_wants_control(void) { return H.wantControl; }
const char* host_control_code(void) { return H.ctrlCode; }

void host_exit(void)
{
	if (H.hget) {
		http_get_close(H.hget);
		H.hget = NULL;
	}
	H.active = false;
	H.wantExit = false;
	H.wantControl = false;
}

// ------------------------------------------------------------------ parsing

static bool host_parse_list(void)
{
	static JsDoc doc;
	if (!json_parse(&doc, H.buf)) return false;
	int arr = json_obj_get(&doc, 0, "quizzes");
	int n = json_arr_len(&doc, arr);
	if (n <= 0) return false;
	if (n > HOST_MAX_QUIZZES) n = HOST_MAX_QUIZZES;
	H.nQuizzes = n;
	for (int i = 0; i < n; i++) {
		int el = json_arr_at(&doc, arr, i);
		snprintf(H.quizzes[i].name, HOST_NAME_LEN, "%s",
		         json_ostr(&doc, el, "name", ""));
		H.quizzes[i].banner = json_obool(&doc, el, "banner", false);
	}
	return true;
}

// The active list count (filtered or full) and the real quiz index for a
// position in the list.
static int host_count(void)
{
	return H.search[0] ? H.nFidx : H.nQuizzes;
}

static int host_real(int pos)
{
	return (H.search[0] && pos >= 0 && pos < H.nFidx) ? H.fidx[pos] : pos;
}

// Rebuilds the search filter (case-insensitive substring match).
static void host_rebuild_filter(void)
{
	H.nFidx = 0;
	if (!H.search[0]) return;
	for (int i = 0; i < H.nQuizzes; i++) {
		if (strcasestr(H.quizzes[i].name, H.search)) {
			if (H.nFidx < HOST_MAX_QUIZZES) H.fidx[H.nFidx++] = i;
		}
	}
}

// Search-box keyboard: empty input is allowed (clears the filter), and the
// left button is "Clear".
static bool host_keyboard_any(const char* hint, char* out, int outsz)
{
	SwkbdState sw;
	swkbdInit(&sw, SWKBD_TYPE_NORMAL, 2, 150);
	swkbdSetHintText(&sw, hint);
	if (out[0]) swkbdSetInitialText(&sw, out);
	swkbdSetButton(&sw, SWKBD_BUTTON_LEFT, "Clear", false);
	swkbdSetButton(&sw, SWKBD_BUTTON_RIGHT, "Cancel", true);
	char tmp[160];
	SwkbdButton b = swkbdInputText(&sw, tmp, sizeof(tmp));
	if (b == SWKBD_BUTTON_LEFT) {
		out[0] = '\0';
		return true;
	}
	if (b != SWKBD_BUTTON_CONFIRM) return false;
	snprintf(out, outsz, "%s", tmp);
	return true;
}

static bool host_parse_codes(void)
{
	static JsDoc doc;
	if (!json_parse(&doc, H.buf)) return false;
	const char* gc = json_ostr(&doc, 0, "code", "");
	const char* cc = json_ostr(&doc, 0, "control_code", "");
	if (!gc[0] || !cc[0]) {
		snprintf(H.buf, sizeof(H.buf), "%s",
		         json_ostr(&doc, 0, "error", "Could not host that quiz"));
		return false;
	}
	snprintf(H.gameCode, sizeof(H.gameCode), "%s", gc);
	snprintf(H.ctrlCode, sizeof(H.ctrlCode), "%s", cc);
	return true;
}

// ------------------------------------------------------------------ stepping

void host_step(void)
{
	if (!H.active) return;
	if (H.phase == 4) {
		precache_step();
		if (precache_wants_back()) {
			precache_end();
			H.phase = 1;
		}
		return;
	}
	if (H.phase == 1) {
		host_rebuild_filter();
		thumb_step(); // banner thumbnails load in the list
	}
	if (H.phase == 0 || H.phase == 2) {
		int r = fetch_poll();
		if (r == 0) return;
		if (r < 0) {
			if (!H.hget) fetch_begin(H.fetchPath);
			return;
		}
		if (H.phase == 0) {
			H.phase = host_parse_list() ? 1 : 0;
		} else {
			H.phase = host_parse_codes() ? 3 : 1; // error: back to the list
		}
	}
}

// ------------------------------------------------------------------ input

static void host_input_browse(u32 kDown, touchPosition* touch, bool touched)
{
	if (kDown & KEY_B) {
		H.wantExit = true;
		return;
	}
	int count = host_count();

	// Y = search the list by name (empty clears the filter).
	if (kDown & KEY_Y) {
		char tmp[96];
		snprintf(tmp, sizeof(tmp), "%s", H.search);
		if (host_keyboard_any("Search quizzes", tmp, sizeof(tmp))) {
			snprintf(H.search, sizeof(H.search), "%s", tmp);
			H.sel = 0;
			H.scroll = 0;
			host_rebuild_filter();
		}
		return;
	}

	// Holding Up/Down for 2 seconds starts a fast scroll that only stops
	// when the button is released.
	u32 held = hidKeysHeld();
	bool navHeld = (held & (KEY_UP | KEY_DOWN)) != 0;
	if (navHeld) {
		if (!H.heldNav) {
			H.heldNav = true;
			H.holdAt = osGetTime();
			H.fast = false;
		}
		if (!H.fast && osGetTime() - H.holdAt >= 2000) H.fast = true;
		if (H.fast && osGetTime() >= H.nextRepeat) {
			H.nextRepeat = osGetTime() + 90;
			if (held & KEY_UP) H.sel = H.sel >= 2 ? H.sel - 2 : 0;
			if ((held & KEY_DOWN) && H.sel + 1 < count) {
				H.sel = H.sel + 2 < count ? H.sel + 2 : count - 1;
			}
		}
	} else if (H.heldNav) {
		H.heldNav = false;
		H.fast = false;
	}

	// Taps: Up/Down move one row, Left/Right move one tile (so both columns
	// are reachable).
	if (kDown & KEY_UP) H.sel = H.sel >= 2 ? H.sel - 2 : 0;
	else if ((kDown & KEY_DOWN) && H.sel + 1 < count) {
		H.sel = H.sel + 2 < count ? H.sel + 2 : count - 1;
	}
	if ((kDown & KEY_LEFT) && H.sel > 0) H.sel--;
	if ((kDown & KEY_RIGHT) && H.sel + 1 < count) H.sel++;

	// Circle pad: single-tile grid movement with auto-repeat. (The 3DS
	// circle pad reports up as positive dy.)
	{
		circlePosition cp;
		hidCircleRead(&cp);
		int dir = 0; // 1 left, 2 right, 3 up, 4 down
		if (cp.dx > 40) dir = 2;
		else if (cp.dx < -40) dir = 1;
		if (cp.dy > 40) dir = 3;
		else if (cp.dy < -40) dir = 4;
		if (dir != H.cpDir) {
			H.cpDir = dir;
			H.cpRepeatAt = osGetTime(); // move immediately
		}
		if (dir && osGetTime() >= H.cpRepeatAt) {
			H.cpRepeatAt = osGetTime() + 150;
			if (dir == 1 && H.sel > 0) H.sel--;
			if (dir == 2 && H.sel + 1 < count) H.sel++;
			if (dir == 3) H.sel = H.sel >= 2 ? H.sel - 2 : 0;
			if (dir == 4 && H.sel + 1 < count) {
				H.sel = H.sel + 2 < count ? H.sel + 2 : count - 1;
			}
		}
	}

	// Keep the selection inside the visible 2x3 tile window.
	if (H.sel < H.scroll) H.scroll = (H.sel / HOST_COLS) * HOST_COLS;
	if (H.sel >= H.scroll + HOST_COLS * HOST_ROWS) {
		H.scroll = ((H.sel - HOST_COLS * (HOST_ROWS - 1)) / HOST_COLS) * HOST_COLS;
	}

	bool open = false;
	if (touched) {
		int tx = touch->px, ty = touch->py;
		float pageH = HOST_ROWS * (HOST_TILE_H + HOST_GAPY);
		if (ty >= 26 && ty < 26 + pageH) {
			int row = (int)((ty - 26) / (HOST_TILE_H + HOST_GAPY));
			int col = (tx < 10 + HOST_TILE_W + HOST_GAPX / 2) ? 0 : 1;
			int pos = H.scroll + row * HOST_COLS + col;
			if (pos >= 0 && pos < count) {
				H.sel = pos;
				open = true;
			}
		}
	}
	if (kDown & KEY_A) open = true;
	// X+SELECT (SELECT pressed while X is held): cache EVERY quiz. Armed on
	// SELECT release so key auto-repeat cannot restart it every 400ms.
	if (hidKeysUp() & KEY_SELECT) H.comboArm = true;
	if (H.comboArm && (hidKeysHeld() & KEY_X) && (kDown & KEY_SELECT)) {
		H.comboArm = false;
		H.phase = 4;
		precache_begin_all(H.host, H.port);
		return;
	}
	if (kDown & KEY_X) {
		// Pre-cache every photo/GIF in this quiz onto the SD card.
		H.phase = 4;
		precache_begin(H.host, H.port, H.quizzes[host_real(H.sel)].name);
		return;
	}
	if (!open) return;

	char enc[HOST_NAME_LEN * 4];
	url_encode(enc, sizeof(enc), H.quizzes[host_real(H.sel)].name);
	char path[HOST_NAME_LEN * 4 + 16];
	snprintf(path, sizeof(path), "/api/host/%s", enc);
	H.phase = 2;
	fetch_begin(path);
}

static void host_input_codes(u32 kDown, touchPosition* touch, bool touched)
{
	if (kDown & KEY_B) {
		H.phase = 1; // back to the list
		return;
	}
	if (touched) {
		int tx = touch->px, ty = touch->py;
		if (ui_hit(20, 176, BOT_W - 40, 44, tx, ty)) {
			H.wantControl = true; // main.c opens the control panel
		} else if (ui_hit(20, 226, BOT_W - 40, 12, tx, ty)) {
			// no-op strip; done handled by B
		}
	}
	if (kDown & KEY_A) H.wantControl = true;
}

void host_input(u32 kDown, touchPosition* touch, bool touched)
{
	if (!H.active) return;
	switch (H.phase) {
	case 0:
	case 2:
		if (kDown & KEY_B) H.wantExit = true;
		break;
	case 1: host_input_browse(kDown, touch, touched); break;
	case 3: host_input_codes(kDown, touch, touched); break;
	case 4: precache_input(kDown, touch, touched);   break;
	}
}

// ------------------------------------------------------------------ drawing

static void host_draw_spinner(const char* text)
{
	ui_text_center_shadow(text, TOP_W / 2, 84, 0.8f, CLR_WHITE);
	float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
	ui_bar(120, 130, TOP_W - 240, 6, pct, CLR_WHITE, RGBA(255, 255, 255, 60));
	ui_text_center("Press B to cancel", TOP_W / 2, 150, 0.45f, CLR_DIM);
}

void host_draw_top(void)
{
	char buf[200];
	char lines[UI_MAX_LINES][UI_LINE_MAX];

	switch (H.phase) {
	case 0: host_draw_spinner("Loading quizzes..."); break;
	case 2: host_draw_spinner("Hosting quiz..."); break;

	case 1: {
		ui_text_center_shadow("HOST A QUIZ", TOP_W / 2, 44, 1.0f, CLR_WHITE);
		if (H.search[0]) {
			snprintf(buf, sizeof(buf), "%d of %d quizzes match \"%s\"",
			         H.nFidx, H.nQuizzes, H.search);
			ui_text_center(buf, TOP_W / 2, 92, 0.45f, CLR_SOFT);
		} else {
			snprintf(buf, sizeof(buf), "%d quizzes on the server", H.nQuizzes);
			ui_text_center(buf, TOP_W / 2, 92, 0.5f, CLR_SOFT);
		}
		snprintf(buf, sizeof(buf), "Selected: %s", H.quizzes[host_real(H.sel)].name);
		float scale = 0.55f;
		int n = ui_wrap_fit(buf, TOP_W - 60, 70, 0.6f, 0.36f, lines, 3, &scale);
		for (int i = 0; i < n; i++) {
			ui_text_center(lines[i], TOP_W / 2, 118 + i * ui_line_height(scale),
			               scale, CLR_GOLD);
		}
		ui_text_center("Circle Pad / D-pad = move   Hold Up/Down = fast scroll", TOP_W / 2, 200, 0.45f, CLR_DIM);
		ui_text_center("A = host  Y = search  X = pre-cache  B = back", TOP_W / 2, 220, 0.4f, CLR_DIM);
		break;
	}

	case 4:
		precache_draw_top();
		break;

	case 3: {
		ui_text_center_shadow("NOW HOSTING", TOP_W / 2, 40, 0.9f, CLR_WHITE);
		snprintf(buf, sizeof(buf), "%s", H.quizzes[H.sel].name);
		float scale = 0.5f;
		int n = ui_wrap_fit(buf, TOP_W - 60, 34, 0.55f, 0.34f, lines, 2, &scale);
		for (int i = 0; i < n; i++) {
			ui_text_center(lines[i], TOP_W / 2, 74 + i * ui_line_height(scale),
			               scale, CLR_SOFT);
		}
		snprintf(buf, sizeof(buf), "GAME PIN: %s", H.gameCode);
		ui_text_center_shadow(buf, TOP_W / 2, 130, 0.8f, CLR_GOLD);
		snprintf(buf, sizeof(buf), "CONTROL CODE: %s", H.ctrlCode);
		ui_text_center(buf, TOP_W / 2, 162, 0.55f, CLR_WHITE);
		ui_text_center("Players join with the game PIN", TOP_W / 2, 206, 0.42f, CLR_DIM);
		break;
	}
	}
}

void host_draw_bottom(void)
{
	switch (H.phase) {
	case 0:
		ui_text_center("Fetching the quiz list...", BOT_W / 2, 110, 0.5f, CLR_SOFT);
		break;

	case 2:
		ui_text_center("Telling the server to host...", BOT_W / 2, 110, 0.5f, CLR_SOFT);
		break;

	case 1: {
		int count = host_count();
		char lines[UI_MAX_LINES][UI_LINE_MAX];
		ui_text("QUIZZES", 14, 8, 0.5f, CLR_SOFT);
		ui_text_right("A = host  Y = search", BOT_W - 14, 10, 0.38f, CLR_DIM);
		for (int v = 0; v < HOST_COLS * HOST_ROWS; v++) {
			int pos = H.scroll + v;
			if (pos >= count) break;
			int idx = host_real(pos);
			int r = v / HOST_COLS, c = v % HOST_COLS;
			float x = 10 + c * (HOST_TILE_W + HOST_GAPX);
			float y = 26 + r * (HOST_TILE_H + HOST_GAPY);
			bool sel = (pos == H.sel);
			if (sel) {
				C2D_DrawRectSolid(x - 2, y - 2, 0.08f,
				                  HOST_TILE_W + 4, HOST_TILE_H + 4, CLR_WHITE);
			}
			u32 fill = sel ? ui_fade(CLR_GOLD, 0.30f) : CLR_GLASS;
			ui_tile(x, y, HOST_TILE_W, HOST_TILE_H, fill, false);

			// Banner photo on top: a down-arrow while it downloads, or a
			// white ? on dark gray when the quiz has no banner at all.
			if (H.quizzes[idx].banner) {
				char enc[HOST_NAME_LEN * 4];
				url_encode(enc, sizeof(enc), H.quizzes[idx].name);
				char path[HOST_NAME_LEN * 4 + 24];
				snprintf(path, sizeof(path), "/api/quizbanner/%s", enc);
				thumb_request(idx, path);
			}
			thumb_draw(idx, x + 4, y + 4, HOST_TILE_W - 8, 30,
			           H.quizzes[idx].banner);

			float sc = 0.42f;
			int nl = ui_wrap_fit(H.quizzes[idx].name, HOST_TILE_W - 12, 18,
			                      0.42f, 0.26f, lines, 2, &sc);
			for (int k = 0; k < nl; k++) {
				ui_text_center(lines[k], x + HOST_TILE_W / 2,
				               y + 36 + k * ui_line_height(sc), sc,
				               sel ? CLR_GOLD : CLR_WHITE);
			}
		}
		if (count == 0) {
			ui_text_center(H.search[0] ? "No quizzes match" : "No quizzes found",
			               BOT_W / 2, 110, 0.5f, CLR_DIM);
		}
		break;
	}

	case 3: {
		ui_tile(20, 176, BOT_W - 40, 44, RGB(56, 214, 160), false);
		ui_text_center("OPEN CONTROL PANEL", BOT_W / 2, 190, 0.55f, CLR_INK);
		ui_text_center("B = back to the quiz list", BOT_W / 2, 230, 0.38f, CLR_DIM);
		break;
	}

	case 4:
		precache_draw_bottom();
		break;
	}
}
