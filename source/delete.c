// Delete-a-quiz. See delete.h.

#include "delete.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "ui.h"
#include "ws.h"

#define DEL_MAX_QUIZZES 128
#define DEL_NAME_LEN    96
#define DEL_VISIBLE     8
#define DEL_ROW_H       24.0f

// Phases: 0 fetching list, 1 browsing, 2 confirming, 3 deleting.
typedef struct {
	char name[DEL_NAME_LEN];
} DelQuiz;

typedef struct {
	bool  active;
	bool  wantExit;
	int   phase;
	char  host[80];
	int   port;
	int   nQuizzes;
	DelQuiz quizzes[DEL_MAX_QUIZZES];
	int   sel;
	int   scroll;
	char  err[200];
	HttpGet* hget;
	char  buf[4096];
} Del;

static Del D;

// ------------------------------------------------------------------ helpers

static void del_fetch_list(void)
{
	if (D.hget) {
		http_get_close(D.hget);
		D.hget = NULL;
	}
	D.hget = http_get_begin(D.host, D.port, "/api/myquizzes", 8000);
}

static int del_fetch_poll(void)
{
	if (!D.hget) return -1;
	int r = http_get_step(D.hget);
	if (r == 0) return 0;
	int ok = 0;
	if (r == 1) {
		char* body = http_get_take_body(D.hget);
		if (body) {
			snprintf(D.buf, sizeof(D.buf), "%s", body);
			free(body);
			ok = 1;
		}
	}
	http_get_close(D.hget);
	D.hget = NULL;
	return ok ? 1 : -1;
}

static bool del_parse_list(void)
{
	static JsDoc doc;
	if (!json_parse(&doc, D.buf)) return false;
	int arr = json_obj_get(&doc, 0, "quizzes");
	int n = json_arr_len(&doc, arr);
	if (n > DEL_MAX_QUIZZES) n = DEL_MAX_QUIZZES;
	D.nQuizzes = n;
	for (int i = 0; i < n; i++) {
		snprintf(D.quizzes[i].name, DEL_NAME_LEN, "%s",
		         json_str(&doc, json_arr_at(&doc, arr, i), ""));
	}
	return true;
}

static void del_post_delete(void)
{
	char enc[DEL_NAME_LEN * 4], body[DEL_NAME_LEN * 4 + 32];
	char* d = enc;
	for (const char* s = D.quizzes[D.sel].name; *s && d < enc + sizeof(enc) - 4; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			*d++ = (char)c;
		} else {
			*d++ = '%';
			*d++ = "0123456789ABCDEF"[c >> 4];
			*d++ = "0123456789ABCDEF"[c & 0x0F];
		}
	}
	*d = '\0';
	snprintf(body, sizeof(body), "quiz_name=%s", enc);
	if (D.hget) {
		http_get_close(D.hget);
		D.hget = NULL;
	}
	D.hget = http_post_begin(D.host, D.port, "/api/deletequiz", body, 8000);
}

// ------------------------------------------------------------------ lifecycle

void delete_start(const char* host, int port)
{
	delete_exit();
	memset(&D, 0, sizeof(D));
	snprintf(D.host, sizeof(D.host), "%s", host);
	D.port = port;
	D.phase = 0;
	D.active = true;
	del_fetch_list();
}

bool delete_wants_exit(void) { return D.wantExit; }

void delete_exit(void)
{
	if (D.hget) {
		http_get_close(D.hget);
		D.hget = NULL;
	}
	D.active = false;
	D.wantExit = false;
}

// ------------------------------------------------------------------ stepping

void delete_step(void)
{
	if (!D.active) return;
	if (D.phase == 0) {
		int r = del_fetch_poll();
		if (r == 0) return;
		if (r < 0 || !del_parse_list()) {
			D.nQuizzes = 0;
			snprintf(D.err, sizeof(D.err), "Could not load your quizzes");
		}
		D.phase = 1;
	} else if (D.phase == 3) {
		int r = del_fetch_poll();
		if (r == 0) return;
		// Refresh the list.
		D.phase = 0;
		del_fetch_list();
	}
}

// ------------------------------------------------------------------ input

void delete_input(u32 kDown, touchPosition* touch, bool touched)
{
	if (!D.active) return;

	if (D.phase == 0) {
		if (kDown & KEY_B) {
			D.wantExit = true;
		}
		return;
	}

	if (D.phase == 1) {
		if (kDown & KEY_B) {
			D.wantExit = true;
			return;
		}
		if (kDown & KEY_UP && D.sel > 0) D.sel--;
		if (kDown & KEY_DOWN && D.sel + 1 < D.nQuizzes) D.sel++;
		if (D.sel < D.scroll) D.scroll = D.sel;
		if (D.sel >= D.scroll + DEL_VISIBLE) D.scroll = D.sel - DEL_VISIBLE + 1;

		bool open = (kDown & KEY_A) != 0;
		if (touched) {
			int ty = touch->py;
			if (ty >= 26 && ty < 26 + DEL_VISIBLE * DEL_ROW_H) {
				int idx = D.scroll + (int)((ty - 26) / DEL_ROW_H);
				if (idx >= 0 && idx < D.nQuizzes) {
					D.sel = idx;
					open = true;
				}
			}
		}
		if (open && D.nQuizzes > 0) D.phase = 2;
		return;
	}

	if (D.phase == 2) {
		if (kDown & KEY_B) {
			D.phase = 1;
			return;
		}
		bool doDel = (kDown & KEY_A) != 0;
		if (touched) {
			int tx = touch->px, ty = touch->py;
			if (ui_hit(20, 130, BOT_W - 40, 42, tx, ty)) {
				doDel = true;
			} else if (ui_hit(20, 184, BOT_W - 40, 42, tx, ty)) {
				D.phase = 1;
				return;
			}
		}
		if (doDel) {
			D.phase = 3;
			del_post_delete();
		}
		return;
	}

	if (D.phase == 3) {
		if (kDown & KEY_B) {
			// Cancel the pending delete request.
			D.phase = 1;
		}
	}
}

// ------------------------------------------------------------------ drawing

void delete_draw_top(void)
{
	char buf[200];

	switch (D.phase) {
	case 0:
		ui_text_center_shadow("DELETING...", TOP_W / 2, 90, 0.8f, CLR_WHITE);
		break;

	case 1: {
		ui_text_center_shadow("DELETE A QUIZ", TOP_W / 2, 44, 0.9f, CLR_WHITE);
		snprintf(buf, sizeof(buf), "%d quizzes you can delete", D.nQuizzes);
		ui_text_center(buf, TOP_W / 2, 92, 0.5f, CLR_SOFT);
		if (D.nQuizzes > 0) {
			snprintf(buf, sizeof(buf), "Selected: %s", D.quizzes[D.sel].name);
			float scale = 0.5f;
			char lines[UI_MAX_LINES][UI_LINE_MAX];
			int n = ui_wrap_fit(buf, TOP_W - 60, 50, 0.55f, 0.34f, lines, 2, &scale);
			for (int i = 0; i < n; i++) {
				ui_text_center(lines[i], TOP_W / 2, 118 + i * ui_line_height(scale),
				               scale, CLR_GOLD);
			}
		}
		ui_text_center("Only quizzes this 3DS uploaded or stole are listed",
		               TOP_W / 2, 200, 0.4f, CLR_DIM);
		ui_text_center("B = back to setup", TOP_W / 2, 220, 0.4f, CLR_DIM);
		break;
	}

	case 2: {
		ui_text_center_shadow("DELETE THIS QUIZ?", TOP_W / 2, 70, 0.8f, CLR_WHITE);
		snprintf(buf, sizeof(buf), "\"%s\"", D.quizzes[D.sel].name);
		float scale = 0.55f;
		char lines[UI_MAX_LINES][UI_LINE_MAX];
		int n = ui_wrap_fit(buf, TOP_W - 60, 70, 0.6f, 0.36f, lines, 3, &scale);
		for (int i = 0; i < n; i++) {
			ui_text_center(lines[i], TOP_W / 2, 108 + i * ui_line_height(scale),
			               scale, CLR_GOLD);
		}
		ui_text_center("This cannot be undone", TOP_W / 2, 190, 0.45f, CLR_BAD);
		break;
	}

	case 3:
		ui_text_center_shadow("DELETING...", TOP_W / 2, 90, 0.8f, CLR_WHITE);
		break;
	}
}

void delete_draw_bottom(void)
{
	switch (D.phase) {
	case 0:
		ui_text_center("Fetching your quizzes...", BOT_W / 2, 110, 0.5f, CLR_SOFT);
		break;

	case 1: {
		ui_text("YOUR QUIZZES", 14, 8, 0.5f, CLR_SOFT);
		ui_text_right("A = delete", BOT_W - 14, 10, 0.38f, CLR_DIM);
		for (int v = 0; v < DEL_VISIBLE; v++) {
			int idx = D.scroll + v;
			if (idx >= D.nQuizzes) break;
			float y = 26 + v * DEL_ROW_H;
			bool sel = (idx == D.sel);
			u32 fill = sel ? ui_fade(CLR_BAD, 0.30f) : CLR_GLASS;
			ui_panel_hi(10, y, BOT_W - 20, DEL_ROW_H - 3, fill);
			ui_text(D.quizzes[idx].name, 18, y + 5, 0.46f,
			        sel ? CLR_BAD : CLR_WHITE);
		}
		if (D.nQuizzes == 0) {
			ui_text_center(D.err[0] ? D.err : "Nothing to delete",
			               BOT_W / 2, 110, 0.5f, CLR_DIM);
		}
		break;
	}

	case 2: {
		ui_tile(20, 130, BOT_W - 40, 42, RGB(255, 92, 116), false);
		ui_text_center("DELETE", BOT_W / 2, 143, 0.58f, CLR_WHITE);
		ui_tile(20, 184, BOT_W - 40, 42, CLR_GLASS_HI, false);
		ui_text_center("CANCEL", BOT_W / 2, 197, 0.58f, CLR_WHITE);
		break;
	}

	case 3:
		ui_text_center("Talking to the server...", BOT_W / 2, 110, 0.5f, CLR_SOFT);
		break;
	}
}
