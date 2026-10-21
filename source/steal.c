// Steal-a-quiz. See steal.h.

#include "steal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "solo.h" // img_request_raw / img_is_loaded / img_progress / img_draw_at
#include "thumb.h"
#include "ui.h"
#include "ws.h"

// Phases: 0 about/input, 1 stealing, 2 renamed page, 3 search results.
typedef struct {
	bool  active;
	bool  wantExit;
	int   phase;
	int   postKind;       // 0 steal, 1 rename, 2 delete, 3 search, 4 more
	char  host[80];
	int   port;
	char  input[160];     // ID / link / search term
	char  quizName[96];
	char  err[200];
	int   nResults;
	struct { char uuid[48]; char title[96]; char cover[400]; } results[64];
	int   cursor;
	int   searchCount;   // results requested by the running search (20 or 5)
	int   sel;
	int   previewIdx;    // result whose big cover is on the top screen
	int   scroll;
	bool  appendMode;
	HttpGet* hget;
	char  buf[16384]; // search results now include cover URLs
} Steal;

static Steal St;

// ------------------------------------------------------------------ helpers

// Form-urlencodes `src` into `out`.
static void form_encode(char* out, int outsz, const char* src)
{
	static const char* hex = "0123456789ABCDEF";
	char* d = out;
	for (const char* s = src; *s && d < out + outsz - 4; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
		    c == '~' || c == ':' || c == '/' || c == '?') {
			*d++ = (char)c;
		} else {
			*d++ = '%';
			*d++ = hex[c >> 4];
			*d++ = hex[c & 0x0F];
		}
	}
	*d = '\0';
}

static void post_begin(const char* path, const char* body)
{
	if (St.hget) {
		http_get_close(St.hget);
		St.hget = NULL;
	}
	St.hget = http_post_begin(St.host, St.port, path, body, 15000);
}

// 1 done (body in St.buf), 0 busy, -1 failed.
static int post_poll(void)
{
	if (!St.hget) return -1;
	int r = http_get_step(St.hget);
	if (r == 0) return 0;
	int ok = 0;
	if (r == 1) {
		char* body = http_get_take_body(St.hget);
		if (body) {
			snprintf(St.buf, sizeof(St.buf), "%s", body);
			free(body);
			ok = 1;
		}
	}
	http_get_close(St.hget);
	St.hget = NULL;
	return ok ? 1 : -1;
}

// ------------------------------------------------------------------ lifecycle

void steal_start(const char* host, int port)
{
	steal_exit();
	memset(&St, 0, sizeof(St));
	snprintf(St.host, sizeof(St.host), "%s", host);
	St.port = port;
	St.phase = 0;
	St.previewIdx = -1;
	St.active = true;
	thumb_begin(St.host, St.port);
	img_request(NULL);
}

bool steal_wants_exit(void) { return St.wantExit; }

void steal_exit(void)
{
	if (St.hget) {
		http_get_close(St.hget);
		St.hget = NULL;
	}
	St.active = false;
	St.wantExit = false;
}

// ------------------------------------------------------------------ parsing

static bool steal_parse_result(void)
{
	static JsDoc doc;
	if (!json_parse(&doc, St.buf)) {
		snprintf(St.err, sizeof(St.err), "The server sent a bad reply");
		return false;
	}
	const char* name = json_ostr(&doc, 0, "quiz_name", "");
	if (name[0]) {
		snprintf(St.quizName, sizeof(St.quizName), "%s", name);
		return true;
	}
	snprintf(St.err, sizeof(St.err), "%s",
	         json_ostr(&doc, 0, "error", "The steal failed"));
	return false;
}

static bool steal_parse_search(void)
{
	static JsDoc doc;
	if (!json_parse(&doc, St.buf)) return false;
	int arr = json_obj_get(&doc, 0, "results");
	int n = json_arr_len(&doc, arr);
	if (!St.appendMode) {
		St.nResults = 0;
		St.sel = 0;
		St.scroll = 0;
	}
	for (int i = 0; i < n && St.nResults < 64; i++) {
		int r = json_arr_at(&doc, arr, i);
		snprintf(St.results[St.nResults].uuid, sizeof(St.results[0].uuid), "%s",
		         json_ostr(&doc, r, "uuid", ""));
		snprintf(St.results[St.nResults].title, sizeof(St.results[0].title), "%s",
		         json_ostr(&doc, r, "title", ""));
		snprintf(St.results[St.nResults].cover, sizeof(St.results[0].cover), "%s",
		         json_ostr(&doc, r, "cover", ""));
		St.nResults++;
	}
	St.cursor = (int)json_onum(&doc, 0, "next_cursor", St.cursor);
	return true;
}

static bool uuid_like(const char* s)
{
	int n = 0;
	for (const char* p = s; *p; p++) {
		char c = *p;
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
		    (c >= 'A' && c <= 'F') || c == '-') {
			n++;
		} else {
			return false;
		}
	}
	return n >= 10;
}

// ------------------------------------------------------------------ stepping

void steal_step(void)
{
	if (!St.active) return;
	if (St.phase == 3) {
		thumb_step(); // cover thumbnails load in the list
		// The selected quiz's cover downloads at full size (256x96) so the
		// top screen can preview it while hovering.
		if (St.sel != St.previewIdx) {
			St.previewIdx = St.sel;
			if (St.sel >= 0 && St.sel < St.nResults && St.results[St.sel].cover[0]) {
				char enc[600];
				form_encode(enc, sizeof(enc), St.results[St.sel].cover);
				char path[640];
				snprintf(path, sizeof(path), "/api/kahootcover?url=%s&size=256", enc);
				img_request_raw(path);
			} else {
				img_request(NULL);
			}
		}
	}
	if (St.phase == 1) {
		int r = post_poll();
		if (r == 0) return;
		if (St.postKind == 2) {
			// Delete request: done either way; leave the page.
			St.wantExit = true;
			return;
		}
		if (r < 0) {
			if (St.postKind == 1) {
				snprintf(St.err, sizeof(St.err), "Could not reach the server");
				St.phase = 2;
			} else if (St.postKind == 3 || St.postKind == 4) {
				snprintf(St.err, sizeof(St.err), "Search failed - check the server");
				St.phase = 0;
			} else {
				St.err[0] = '\0';
				St.phase = 0;
			}
			return;
		}
		if (St.postKind == 3 || St.postKind == 4) {
			if (!steal_parse_search() || St.nResults == 0) {
				if (!St.appendMode) {
					snprintf(St.err, sizeof(St.err), "No quizzes found");
					St.phase = 0;
				} else {
					snprintf(St.err, sizeof(St.err), "No more results");
				}
				return;
			}
			St.err[0] = '\0';
			St.phase = 3;
			return;
		}
		if (steal_parse_result()) {
			St.phase = 2;
		} else if (St.postKind == 1) {
			St.phase = 2; // show the error next to the name
		} else {
			St.phase = 0; // show the error on the input page
		}
	}
}

// ------------------------------------------------------------------ input

static bool steal_keyboard(const char* hint, const char* initial,
                           char* out, int outsz)
{
	SwkbdState sw;
	swkbdInit(&sw, SWKBD_TYPE_NORMAL, 2, 150);
	swkbdSetHintText(&sw, hint);
	if (initial && *initial) swkbdSetInitialText(&sw, initial);
	swkbdSetValidation(&sw, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	char tmp[160];
	if (swkbdInputText(&sw, tmp, sizeof(tmp)) != SWKBD_BUTTON_CONFIRM) return false;
	snprintf(out, outsz, "%s", tmp);
	return true;
}

static void steal_do_steal(void)
{
	if (!St.input[0]) {
		snprintf(St.err, sizeof(St.err), "Enter a Kahoot ID, link, or name first");
		return;
	}
	St.err[0] = '\0';
	char enc[480];
	form_encode(enc, sizeof(enc), St.input);
	char body[512];

	if (uuid_like(St.input)) {
		// Direct steal by UUID.
		snprintf(body, sizeof(body), "input=%s", enc);
		St.postKind = 0;
		St.phase = 1;
		post_begin("/api/stealquiz", body);
	} else {
		// Search; the results page lets the user pick one.
		snprintf(body, sizeof(body), "q=%s&cursor=0&count=20", enc);
		St.appendMode = false;
		St.postKind = 3;
		St.searchCount = 20;
		St.phase = 1;
		post_begin("/api/searchkahoot", body);
	}
}

static void steal_do_more(void)
{
	St.err[0] = '\0';
	char enc[480], body[512];
	form_encode(enc, sizeof(enc), St.input);
	snprintf(body, sizeof(body), "q=%s&cursor=%d&count=5", enc, St.cursor);
	St.appendMode = true;
	St.postKind = 4;
	St.searchCount = 5;
	St.phase = 1;
	post_begin("/api/searchkahoot", body);
}

static void steal_do_pick(int idx)
{
	if (idx < 0 || idx >= St.nResults) return;
	St.err[0] = '\0';
	char enc[160], body[192];
	form_encode(enc, sizeof(enc), St.results[idx].uuid);
	snprintf(body, sizeof(body), "input=%s", enc);
	St.postKind = 0;
	St.phase = 1;
	post_begin("/api/stealquiz", body);
}

static void steal_do_rename(void)
{
	char newName[96];
	if (!steal_keyboard("New quiz name", St.quizName, newName, sizeof(newName))) {
		return;
	}
	if (!strcmp(newName, St.quizName)) return;

	St.err[0] = '\0';
	char encQ[320], encN[320], body[640];
	form_encode(encQ, sizeof(encQ), St.quizName);
	form_encode(encN, sizeof(encN), newName);
	snprintf(body, sizeof(body), "quiz_name=%s&new_name=%s", encQ, encN);

	St.postKind = 1;
	St.phase = 1;
	post_begin("/api/renamequiz", body);
}

void steal_input(u32 kDown, touchPosition* touch, bool touched)
{
	if (!St.active) return;

	if (St.phase == 0) {
		if (kDown & KEY_B) {
			St.wantExit = true;
			return;
		}
		if (touched) {
			int tx = touch->px, ty = touch->py;
			// Input box
			if (ui_hit(20, 96, BOT_W - 40, 46, tx, ty)) {
				steal_keyboard("Kahoot ID, link, or name", St.input,
				               St.input, sizeof(St.input));
			} else if (ui_hit(20, 176, BOT_W - 40, 46, tx, ty)) {
				steal_do_steal();
			}
		}
		if (kDown & KEY_A) steal_do_steal();
	} else if (St.phase == 3) {
		if (kDown & KEY_B) {
			St.phase = 0;
			img_request(NULL);
			return;
		}
		if (kDown & KEY_UP && St.sel > 0) St.sel--;
		if (kDown & KEY_DOWN && St.sel + 1 < St.nResults) St.sel++;
		// This page keeps the old jump: Left goes up 5, Right goes down 5.
		if (kDown & KEY_LEFT) St.sel = St.sel >= 5 ? St.sel - 5 : 0;
		if ((kDown & KEY_RIGHT) && St.sel + 1 < St.nResults) {
			St.sel = St.sel + 5 < St.nResults ? St.sel + 5 : St.nResults - 1;
		}
		if (St.sel < St.scroll) St.scroll = St.sel;
		if (St.sel >= St.scroll + 8) St.scroll = St.sel - 8 + 1;

		bool pick = (kDown & KEY_A) != 0;
		bool more = false;
		if (touched) {
			int tx = touch->px, ty = touch->py;
			if (ty >= 26 && ty < 26 + 8 * 24.0f) {
				int row = (int)((ty - 26) / 24.0f);
				int idx = St.scroll + row;
				if (idx >= 0 && idx < St.nResults) {
					St.sel = idx;
					pick = true;
				}
			} else if (ui_hit(20, 216, BOT_W - 40, 20, tx, ty)) {
				more = true;
			}
		}
		if (pick) steal_do_pick(St.sel);
		else if (more) steal_do_more();
	} else if (St.phase == 2) {
		if (kDown & KEY_B) {
			// Discard the stolen quiz entirely.
			char encQ[320], body[384];
			form_encode(encQ, sizeof(encQ), St.quizName);
			snprintf(body, sizeof(body), "quiz_name=%s", encQ);
			St.postKind = 2;
			St.phase = 1;
			post_begin("/api/deletequiz", body);
			return;
		}
		if (touched) {
			int tx = touch->px, ty = touch->py;
			if (ui_hit(20, 120, BOT_W - 40, 44, tx, ty)) {
				steal_do_rename();
			} else if (ui_hit(20, 176, BOT_W - 40, 46, tx, ty)) {
				St.wantExit = true; // saved; done
			}
		}
		if (kDown & KEY_A) St.wantExit = true;
	}
}

// ------------------------------------------------------------------ drawing

void steal_draw_top(void)
{
	char lines[UI_MAX_LINES][UI_LINE_MAX];
	char buf[200];

	switch (St.phase) {
	case 0: {
		ui_text_center_shadow("STEAL A QUIZ", TOP_W / 2, 40, 0.9f, CLR_WHITE);
		static const char* about =
		    "Grab a public Kahoot quiz by ID, link, or name. The server "
		    "converts it (questions AND images) into a QuizFabric quiz that "
		    "becomes yours to edit and host.";
		float scale = 0.5f;
		int n = ui_wrap_fit(about, TOP_W - 60, 100, 0.55f, 0.36f, lines, 6, &scale);
		for (int i = 0; i < n; i++) {
			ui_text_center(lines[i], TOP_W / 2, 76 + i * ui_line_height(scale),
			               scale, CLR_SOFT);
		}
		if (St.err[0]) {
			snprintf(buf, sizeof(buf), "%s", St.err);
			ui_text_center(buf, TOP_W / 2, 208, 0.42f, CLR_BAD);
		}
		break;
	}

	case 1:
		if (St.postKind == 3 || St.postKind == 4) {
			// A search POST returns every result in one body, so its
			// download progress maps onto the result count (20 or 5).
			ui_text_center_shadow("FINDING QUIZZES", TOP_W / 2, 84, 0.8f, CLR_WHITE);
			float p = St.hget ? http_get_progress(St.hget) : -1.0f;
			if (p >= 0.0f) {
				if (p > 1.0f) p = 1.0f;
				ui_bar(120, 140, TOP_W - 240, 12, p, CLR_GOLD,
				       RGBA(255, 255, 255, 60));
				snprintf(buf, sizeof(buf), "%d out of %d",
				         (int)(p * St.searchCount), St.searchCount);
				ui_text_center(buf, TOP_W / 2, 162, 0.5f, CLR_SOFT);
			} else {
				float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
				ui_bar(120, 140, TOP_W - 240, 12, pct, CLR_GOLD,
				       RGBA(255, 255, 255, 60));
				ui_text_center("talking to Kahoot...", TOP_W / 2, 162, 0.45f, CLR_SOFT);
			}
		} else {
			ui_text_center_shadow("STEALING...", TOP_W / 2, 84, 0.8f, CLR_WHITE);
			ui_text_center("Fetching and converting the quiz", TOP_W / 2, 124, 0.5f, CLR_SOFT);
			{
				float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
				ui_bar(120, 150, TOP_W - 240, 6, pct, CLR_WHITE, RGBA(255, 255, 255, 60));
			}
		}
		break;

	case 2: {
		ui_text_center_shadow("STOLEN!", TOP_W / 2, 40, 0.95f, CLR_GOOD);
		ui_text_center("The quiz is now yours", TOP_W / 2, 84, 0.5f, CLR_SOFT);
		snprintf(buf, sizeof(buf), "Name: %s", St.quizName);
		float scale = 0.55f;
		int n = ui_wrap_fit(buf, TOP_W - 60, 70, 0.6f, 0.36f, lines, 3, &scale);
		for (int i = 0; i < n; i++) {
			ui_text_center(lines[i], TOP_W / 2, 110 + i * ui_line_height(scale),
			               scale, CLR_GOLD);
		}
		ui_text_center("Rename it below, then SAVE (B = discard)", TOP_W / 2, 200, 0.45f, CLR_DIM);
		if (St.err[0]) {
			ui_text_center(St.err, TOP_W / 2, 216, 0.4f, CLR_BAD);
		}
		break;
	}

	case 3: {
		ui_text_center_shadow("SEARCH RESULTS", TOP_W / 2, 36, 0.85f, CLR_WHITE);
		snprintf(buf, sizeof(buf), "\"%s\"", St.input);
		ui_text_center(buf, TOP_W / 2, 72, 0.45f, CLR_DIM);
		snprintf(buf, sizeof(buf), "%d shown - pick one to steal", St.nResults);
		ui_text_center(buf, TOP_W / 2, 96, 0.5f, CLR_SOFT);
		ui_text_center("Left/Right = jump 5", TOP_W / 2, 216, 0.36f, CLR_DIM);

		bool selCover = St.sel >= 0 && St.sel < St.nResults &&
		                St.results[St.sel].cover[0];
		if (selCover && img_is_loaded()) {
			// Big preview of the selected quiz's cover (256x96 download).
			img_draw_at(TOP_W / 2, 112, 300, 96);
		} else {
			// Small thumbnails: how many of the covers on screen are in.
			int total = 0, loaded = 0;
			for (int v = 0; v < 8; v++) {
				int idx = St.scroll + v;
				if (idx >= St.nResults) break;
				if (!St.results[idx].cover[0]) continue;
				total++;
				if (thumb_loaded(idx)) loaded++;
			}
			if (total > 0 && loaded < total) {
				snprintf(buf, sizeof(buf), "DOWNLOADING IMAGE %d/%d", loaded, total);
				ui_text_center(buf, TOP_W / 2, 124, 0.48f, CLR_SOFT);
				ui_bar(120, 144, TOP_W - 240, 8, (float)loaded / (float)total,
				       CLR_GOLD, RGBA(255, 255, 255, 60));
			}
			if (selCover) {
				// Progress of the big preview download.
				ui_text_center("THIS IMAGE", TOP_W / 2, 162, 0.42f, CLR_DIM);
				float p = img_progress();
				if (p >= 0.0f) {
					if (p > 1.0f) p = 1.0f;
					ui_bar(120, 180, TOP_W - 240, 8, p, CLR_WHITE,
					       RGBA(255, 255, 255, 60));
					snprintf(buf, sizeof(buf), "%d%%", (int)(p * 100.0f));
					ui_text_center(buf, TOP_W / 2, 192, 0.4f, CLR_SOFT);
				} else {
					float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
					ui_bar(120, 180, TOP_W - 240, 8, pct, CLR_WHITE,
					       RGBA(255, 255, 255, 60));
					ui_text_center("connecting...", TOP_W / 2, 192, 0.4f, CLR_DIM);
				}
			}
		}
		if (St.err[0]) {
			ui_text_center(St.err, TOP_W / 2, 210, 0.42f, CLR_BAD);
		}
		break;
	}
	}
}

void steal_draw_bottom(void)
{
	char buf[240];

	switch (St.phase) {
	case 0: {
		ui_text("KAHOOT ID / LINK / NAME", 20, 72, 0.4f, CLR_DIM);
		ui_panel_hi(20, 96, BOT_W - 40, 46, CLR_GLASS);
		if (St.input[0]) {
			snprintf(buf, sizeof(buf), "%.40s", St.input);
			ui_text_center(buf, BOT_W / 2, 112, 0.5f, CLR_WHITE);
		} else {
			ui_text_center("tap to enter", BOT_W / 2, 112, 0.5f, CLR_DIM);
		}
		ui_tile(20, 176, BOT_W - 40, 46, RGB(255, 110, 199), false);
		ui_text_center("STEAL", BOT_W / 2, 190, 0.62f, CLR_INK);
		ui_text_center("B = back", BOT_W / 2, 230, 0.38f, CLR_DIM);
		break;
	}

	case 1:
		ui_text_center("Talking to the server...", BOT_W / 2, 110, 0.5f, CLR_SOFT);
		break;

	case 2: {
		ui_tile(20, 120, BOT_W - 40, 44, RGB(255, 208, 92), false);
		ui_text_center("RENAME", BOT_W / 2, 133, 0.55f, CLR_INK);
		ui_tile(20, 176, BOT_W - 40, 46, RGB(56, 214, 160), false);
		ui_text_center("SAVE", BOT_W / 2, 190, 0.62f, CLR_INK);
		ui_text_center("B = discard this quiz", BOT_W / 2, 230, 0.38f, CLR_DIM);
		break;
	}

	case 3: {
		for (int v = 0; v < 8; v++) {
			int idx = St.scroll + v;
			if (idx >= St.nResults) break;
			float y = 26 + v * 24.0f;
			bool sel = (idx == St.sel);
			u32 fill = sel ? ui_fade(CLR_GOLD, 0.30f) : CLR_GLASS;
			ui_panel_hi(10, y, BOT_W - 20, 21, fill);
			// Kahoot cover photo on the left (single column - no grid):
			// down-arrow while it downloads, ? when there is no cover.
			if (St.results[idx].cover[0]) {
				char enc[600];
				form_encode(enc, sizeof(enc), St.results[idx].cover);
				char path[640];
				snprintf(path, sizeof(path), "/api/kahootcover?url=%s", enc);
				thumb_request(idx, path);
			}
			thumb_draw(idx, 14, y + 2.5f, 18, 16,
			           St.results[idx].cover[0] != 0);
			ui_text(St.results[idx].title, 38, y + 4, 0.42f,
			        sel ? CLR_GOLD : CLR_WHITE);
		}
		ui_tile(20, 216, BOT_W - 40, 20, RGB(104, 132, 255), false);
		ui_text_center("MORE (+5)", BOT_W / 2, 218, 0.42f, CLR_WHITE);
		break;
	}
	}
}
