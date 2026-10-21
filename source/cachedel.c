// Offline menu: delete cached quizzes from the SD card. See cachedel.h.
//
// Lists every cachedfiles/<quiz> folder that has a quiz.json (offline-
// playable). A deletes the selected quiz's cached files after a confirm;
// X wipes EVERYTHING cached (all quizzes plus partial downloads).

#include "cachedel.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define CD_MAX_QUIZZES 128
#define CD_NAME_LEN 96

typedef struct {
	bool  active;
	bool  wantExit;
	int   phase;       // 0 list, 1 confirm
	int   n;
	char  names[CD_MAX_QUIZZES][CD_NAME_LEN];
	int   sel;
	int   scroll;
} Cd;

static Cd C;

static void cd_scan(void)
{
	C.n = 0;
	DIR* d = opendir("sdmc:/QuizFabric/cachedfiles");
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d)) != NULL && C.n < CD_MAX_QUIZZES) {
		if (e->d_name[0] == '.') continue;
		char qj[420];
		snprintf(qj, sizeof(qj), "sdmc:/QuizFabric/cachedfiles/%s/quiz.json",
		         e->d_name);
		FILE* f = fopen(qj, "rb");
		if (!f) continue;
		fclose(f);
		snprintf(C.names[C.n], CD_NAME_LEN, "%s", e->d_name);
		C.n++;
	}
	closedir(d);
}

// Deletes every cached file for one quiz (folder itself stays; ctrulib has
// no rmdir, and an empty folder is harmless).
static void cd_delete_quiz(int idx)
{
	char dir[400];
	snprintf(dir, sizeof(dir), "sdmc:/QuizFabric/cachedfiles/%s", C.names[idx]);
	DIR* qd = opendir(dir);
	if (!qd) return;
	struct dirent* f;
	while ((f = readdir(qd)) != NULL) {
		if (f->d_name[0] == '.') continue;
		char p[440];
		snprintf(p, sizeof(p), "%s/%s", dir, f->d_name);
		remove(p);
	}
	closedir(qd);
	// Also drop any half-finished download for this quiz.
	DIR* dd = opendir("sdmc:/QuizFabric/downloading");
	if (dd) {
		struct dirent* e;
		while ((e = readdir(dd)) != NULL) {
			if (e->d_name[0] == '.') continue;
			char prefix[128];
			snprintf(prefix, sizeof(prefix), "%s~", C.names[idx]);
			if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) {
				char p[440];
				snprintf(p, sizeof(p), "sdmc:/QuizFabric/downloading/%s", e->d_name);
				remove(p);
			}
		}
		closedir(dd);
	}
}

// Wipes everything cached: all quiz folders and all partial downloads.
static void cd_delete_all(void)
{
	DIR* d = opendir("sdmc:/QuizFabric/downloading");
	if (d) {
		struct dirent* e;
		while ((e = readdir(d)) != NULL) {
			if (e->d_name[0] == '.') continue;
			char p[400];
			snprintf(p, sizeof(p), "sdmc:/QuizFabric/downloading/%s", e->d_name);
			remove(p);
		}
		closedir(d);
	}
	for (int i = 0; i < C.n; i++) cd_delete_quiz(i);
	C.n = 0;
	C.sel = 0;
	C.scroll = 0;
}

void cachedel_start(void)
{
	cachedel_exit();
	memset(&C, 0, sizeof(C));
	C.active = true;
	C.phase = 0;
	cd_scan();
}

bool cachedel_wants_exit(void) { return C.wantExit; }

void cachedel_exit(void)
{
	C.active = false;
	C.wantExit = false;
}

void cachedel_step(void)
{
	if (!C.active) return;
}

void cachedel_input(u32 kDown, touchPosition* touch, bool touched)
{
	if (!C.active) return;

	if (C.phase == 0) {
		if (kDown & KEY_B) {
			C.wantExit = true;
			return;
		}
		if (kDown & KEY_X) {
			// Wipe everything cached.
			cd_delete_all();
			return;
		}
		if (kDown & KEY_UP && C.sel > 0) C.sel--;
		if (kDown & KEY_DOWN && C.sel + 1 < C.n) C.sel++;
		if (C.sel < C.scroll) C.scroll = C.sel;
		if (C.sel >= C.scroll + 8) C.scroll = C.sel - 8 + 1;
		bool pick = (kDown & KEY_A) != 0;
		if (touched) {
			int tx = touch->px, ty = touch->py;
			if (ty >= 26 && ty < 26 + 8 * 24.0f) {
				int row = (int)((ty - 26) / 24.0f);
				int idx = C.scroll + row;
				if (idx >= 0 && idx < C.n) {
					C.sel = idx;
					pick = true;
				}
			}
		}
		if (pick && C.n > 0) C.phase = 1;
	} else if (C.phase == 1) {
		if (kDown & KEY_B) {
			C.phase = 0;
			return;
		}
		if ((kDown & KEY_A) || touched) {
			cd_delete_quiz(C.sel);
			cd_scan();
			if (C.sel >= C.n && C.n > 0) C.sel = C.n - 1;
			C.phase = 0;
		}
	}
}

void cachedel_draw_top(void)
{
	char buf[200];
	char lines[UI_MAX_LINES][UI_LINE_MAX];

	switch (C.phase) {
	case 0:
		if (C.n == 0) {
			ui_text_center_shadow("DELETE CACHED QUIZZES", TOP_W / 2, 60, 0.8f, CLR_WHITE);
			ui_text_center("Nothing is cached yet", TOP_W / 2, 110, 0.55f, CLR_SOFT);
			ui_text_center("Pre-cache quizzes with X (or X+SELECT)", TOP_W / 2, 140, 0.45f, CLR_DIM);
			ui_text_center("to play them offline", TOP_W / 2, 164, 0.45f, CLR_DIM);
		} else {
			ui_text_center_shadow("DELETE CACHED QUIZZES", TOP_W / 2, 60, 0.8f, CLR_WHITE);
			snprintf(buf, sizeof(buf), "%d cached quiz%s", C.n, C.n == 1 ? "" : "zes");
			ui_text_center(buf, TOP_W / 2, 100, 0.5f, CLR_SOFT);
			snprintf(buf, sizeof(buf), "Selected: %s", C.names[C.sel]);
			float scale = 0.55f;
			int n = ui_wrap_fit(buf, TOP_W - 60, 70, 0.6f, 0.36f, lines, 3, &scale);
			for (int i = 0; i < n; i++) {
				ui_text_center(lines[i], TOP_W / 2, 126 + i * ui_line_height(scale),
				               scale, CLR_GOLD);
			}
			ui_text_center("A = delete selected   X = delete everything", TOP_W / 2, 200, 0.42f, CLR_DIM);
		}
		break;

	case 1: {
		ui_text_center_shadow("DELETE CACHED QUIZ?", TOP_W / 2, 70, 0.8f, CLR_BAD);
		snprintf(buf, sizeof(buf), "%s", C.names[C.sel]);
		float scale = 0.55f;
		int n = ui_wrap_fit(buf, TOP_W - 60, 70, 0.6f, 0.36f, lines, 3, &scale);
		for (int i = 0; i < n; i++) {
			ui_text_center(lines[i], TOP_W / 2, 110 + i * ui_line_height(scale),
			               scale, CLR_WHITE);
		}
		ui_text_center("Its cached photos and GIFs will be removed", TOP_W / 2, 176, 0.45f, CLR_SOFT);
		break;
	}
	}
}

void cachedel_draw_bottom(void)
{
	switch (C.phase) {
	case 0: {
		for (int v = 0; v < 8; v++) {
			int idx = C.scroll + v;
			if (idx >= C.n) break;
			float y = 26 + v * 24.0f;
			bool sel = (idx == C.sel);
			u32 fill = sel ? ui_fade(CLR_GOLD, 0.30f) : CLR_GLASS;
			ui_panel_hi(10, y, BOT_W - 20, 21, fill);
			ui_text(C.names[idx], 18, y + 4, 0.42f,
			        sel ? CLR_GOLD : CLR_WHITE);
		}
		if (C.n == 0) {
			ui_text_center("Nothing cached", BOT_W / 2, 110, 0.5f, CLR_DIM);
		}
		ui_text_center("B = back to offline menu", BOT_W / 2, 228, 0.38f, CLR_DIM);
		break;
	}

	case 1: {
		ui_tile(20, 100, BOT_W - 40, 46, RGB(255, 92, 116), false);
		ui_text_center("A = DELETE IT", BOT_W / 2, 115, 0.55f, CLR_WHITE);
		ui_tile(20, 160, BOT_W - 40, 40, CLR_GLASS, false);
		ui_text_center("B = keep it", BOT_W / 2, 171, 0.5f, CLR_WHITE);
		break;
	}
	}
}
