// Solo quiz mode. See solo.h.

#define _GNU_SOURCE 1 // strcasestr in newlib

#include "solo.h"

#include "app.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "json.h"
#include "precache.h"
#include "thumb.h"
#include "ui.h"
#include "ws.h"

#define SOLO_MAX_QUIZZES   128
#define SOLO_MAX_QUESTIONS 200
#define SOLO_MAX_ANSWERS   8
#define SOLO_NAME_LEN      96
#define SOLO_Q_LEN         640
#define SOLO_A_LEN         128

// Quiz grid: 2 tiles per row, 3 rows visible.
#define SOLO_COLS     2
#define SOLO_ROWS     3
#define SOLO_TILE_W   145.0f
#define SOLO_TILE_H   58.0f
#define SOLO_GAPX     10.0f
#define SOLO_GAPY     8.0f

// Phases: 0 fetching list, 1 browsing, 2 fetching quiz, 3 playing, 4 done.
typedef struct {
	char name[SOLO_NAME_LEN];
	bool banner; // server has a banner image for this quiz
} SoloQuiz;

typedef struct {
	char question[SOLO_Q_LEN];
	char answers[SOLO_MAX_ANSWERS][SOLO_A_LEN];
	int  nAnswers;
	int  correct[SOLO_MAX_ANSWERS];
	bool isMC;
	int  qtype;      // 0 mc, 1 type, 2 slider, 3 puzzle
	float sMin, sMax, sAnswer, sTol;
	int  pOrder[SOLO_MAX_ANSWERS];
	char typeAnswer[SOLO_A_LEN];
	char imgUrl[192];
} SoloQ;

typedef struct {
	bool  active;
	bool  wantExit;
	int   phase;
	char  host[80];
	int   port;

	int   nQuizzes;
	SoloQuiz quizzes[SOLO_MAX_QUIZZES];
	int   sel;        // selected quiz position while browsing
	int   scroll;     // first visible tile
	bool  heldNav;    // Up/Down held: becomes fast scroll after 2s
	u64   holdAt;
	bool  fast;
	u64   nextRepeat;
	int   cpDir;      // circle pad direction (1 l, 2 r, 3 up, 4 down)
	u64   cpRepeatAt; // circle pad auto-repeat
	bool  comboArm;   // X+SELECT armed (SELECT must release first)
	char  search[96]; // Y search filter (empty = no filter)
	int   fidx[SOLO_MAX_QUIZZES]; // filtered positions -> real quiz index
	int   nFidx;

	int   nQ;
	SoloQ qs[SOLO_MAX_QUESTIONS];
	int   qi;         // current question
	int   selAns;     // selected answer, -1 none
	bool  revealing;
	int   lastCorrect; // -1 n/a, 0 wrong, 1 right
	int   lastGain;
	int   score;
	char  typed[SOLO_A_LEN];
	float sVal;        // slider answer
	u64   qStart;

	HttpGet* hget;
	char  fetchPath[256];
	char  buf[131072]; // raw JSON bodies (mutated in place by the parser)
} Solo;

static Solo S;
static bool s_offline = false; // offline mode: cached quizzes only

void solo_offline_mode(bool on) { s_offline = on; }

static void solo_error(const char* msg);
static int  solo_real(int pos);
static bool solo_parse_questions(void);
static void solo_start_question(void);

// Builds the offline quiz list from the SD card: every cachedfiles/<quiz>
// folder with a quiz.json (saved by the pre-cache mode).
static void solo_list_offline(void)
{
	S.nQuizzes = 0;
	DIR* d = opendir("sdmc:/QuizFabric/cachedfiles");
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d)) != NULL && S.nQuizzes < SOLO_MAX_QUIZZES) {
		if (e->d_name[0] == '.') continue;
		char qj[420];
		snprintf(qj, sizeof(qj), "sdmc:/QuizFabric/cachedfiles/%s/quiz.json",
		         e->d_name);
		FILE* f = fopen(qj, "rb");
		if (!f) continue;
		fclose(f);
		snprintf(S.quizzes[S.nQuizzes].name, SOLO_NAME_LEN, "%s", e->d_name);
		// Offline banners come from the cached banner.565.
		char bj[420];
		snprintf(bj, sizeof(bj), "sdmc:/QuizFabric/cachedfiles/%s/banner.565",
		         e->d_name);
		FILE* bf = fopen(bj, "rb");
		S.quizzes[S.nQuizzes].banner = (bf != NULL);
		if (bf) fclose(bf);
		S.nQuizzes++;
	}
	closedir(d);
	S.phase = 1;
}

// Opens a cached quiz straight from the SD card (offline play).
static void solo_open_offline(void)
{
	char p[420];
	snprintf(p, sizeof(p), "sdmc:/QuizFabric/cachedfiles/%s/quiz.json",
	         S.quizzes[solo_real(S.sel)].name);
	FILE* f = fopen(p, "rb");
	if (!f) {
		solo_error("That quiz is not cached");
		return;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > (long)sizeof(S.buf) - 1) {
		fclose(f);
		solo_error("Cached quiz too big");
		return;
	}
	fread(S.buf, 1, (size_t)sz, f);
	S.buf[sz] = '\0';
	fclose(f);
	if (!solo_parse_questions()) {
		solo_error("Could not read the cached quiz");
		return;
	}
	S.qi = 0;
	S.score = 0;
	S.phase = 3;
	solo_start_question();
}

// ------------------------------------------------------------------ helpers

static void solo_error(const char* msg)
{
	app_error("%s", msg);
	S.wantExit = true;
}

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
	if (S.hget) {
		http_get_close(S.hget);
		S.hget = NULL;
	}
	snprintf(S.fetchPath, sizeof(S.fetchPath), "%s", path);
	S.hget = http_get_begin(S.host, S.port, S.fetchPath, 8000);
}

// Returns 1 when the fetch finished with body in S.buf, 0 while busy, -1 on
// failure (caller may retry).
static int fetch_poll(void)
{
	if (!S.hget) return -1;
	int r = http_get_step(S.hget);
	if (r == 0) return 0;
	int ok = 0;
	if (r == 1) {
		char* body = http_get_take_body(S.hget);
		if (body) {
			snprintf(S.buf, sizeof(S.buf), "%s", body);
			free(body);
			ok = 1;
		}
	}
	http_get_close(S.hget);
	S.hget = NULL;
	return ok ? 1 : -1;
}

// ------------------------------------------------------------------ lifecycle

void solo_start(const char* host, int port)
{
	solo_exit();
	memset(&S, 0, sizeof(S));
	snprintf(S.host, sizeof(S.host), "%s", host);
	S.port = port;
	S.phase = 0;
	S.sel = 0;
	S.selAns = -1;
	S.lastCorrect = -1;
	S.active = true;
	thumb_begin(S.host, S.port);
	if (s_offline) {
		// Offline: the list comes from the SD card, no server involved.
		solo_list_offline();
		return;
	}
	fetch_begin("/api/quizzes");
}

bool solo_wants_exit(void) { return S.wantExit; }

void solo_exit(void)
{
	if (S.hget) {
		http_get_close(S.hget);
		S.hget = NULL;
	}
	S.active = false;
	S.wantExit = false;
}

// ------------------------------------------------------------------ parsing

static bool solo_parse_list(void)
{
	static JsDoc doc;
	if (!json_parse(&doc, S.buf)) return false;
	int arr = json_obj_get(&doc, 0, "quizzes");
	int n = json_arr_len(&doc, arr);
	if (n <= 0) return false;
	if (n > SOLO_MAX_QUIZZES) n = SOLO_MAX_QUIZZES;
	S.nQuizzes = n;
	for (int i = 0; i < n; i++) {
		int el = json_arr_at(&doc, arr, i);
		snprintf(S.quizzes[i].name, SOLO_NAME_LEN, "%s",
		         json_ostr(&doc, el, "name", ""));
		S.quizzes[i].banner = json_obool(&doc, el, "banner", false);
	}
	return true;
}

static bool solo_parse_questions(void)
{
	static JsDoc doc;
	if (!json_parse(&doc, S.buf)) return false;
	int n = json_arr_len(&doc, 0); // root is the questions array
	if (n <= 0) return false;
	if (n > SOLO_MAX_QUESTIONS) n = SOLO_MAX_QUESTIONS;
	S.nQ = n;
	for (int i = 0; i < n; i++) {
		int qn = json_arr_at(&doc, 0, i);
		SoloQ* q = &S.qs[i];
		snprintf(q->question, SOLO_Q_LEN, "%s",
		         json_ostr(&doc, qn, "question", ""));
		const char* t = json_ostr(&doc, qn, "type", "type");
		if (!strcmp(t, "mc")) q->qtype = 0;
		else if (!strcmp(t, "slider")) q->qtype = 2;
		else if (!strcmp(t, "puzzle")) q->qtype = 3;
		else q->qtype = 1;
		q->isMC = (q->qtype == 0);

		int arr = json_obj_get(&doc, qn, "answers");
		int na = json_arr_len(&doc, arr);
		if (na > SOLO_MAX_ANSWERS) na = SOLO_MAX_ANSWERS;
		q->nAnswers = na;
		for (int k = 0; k < na; k++) {
			snprintf(q->answers[k], SOLO_A_LEN, "%s",
			         json_str(&doc, json_arr_at(&doc, arr, k), ""));
		}
		for (int k = 0; k < SOLO_MAX_ANSWERS; k++) {
			q->correct[k] = 0;
			q->pOrder[k] = k;
		}
		int ci = json_obj_get(&doc, qn, "correct_indices");
		int nc = json_arr_len(&doc, ci);
		for (int k = 0; k < nc; k++) {
			int idx = (int)json_num(&doc, json_arr_at(&doc, ci, k), -1);
			if (idx >= 0 && idx < na) q->correct[idx] = 1;
		}
		snprintf(q->typeAnswer, SOLO_A_LEN, "%s",
		         json_ostr(&doc, qn, "answer", ""));
		q->sMin = (float)json_onum(&doc, qn, "min", 0);
		q->sMax = (float)json_onum(&doc, qn, "max", 100);
		q->sAnswer = (float)json_onum(&doc, qn, "answer", 0);
		q->sTol = (float)json_onum(&doc, qn, "tolerance", 0);

		q->imgUrl[0] = '\0';
		int imgs = json_obj_get(&doc, qn, "images");
		if (json_arr_len(&doc, imgs) > 0) {
			int first = json_arr_at(&doc, imgs, 0);
			snprintf(q->imgUrl, sizeof(q->imgUrl), "%s",
			         json_ostr(&doc, first, "url", ""));
		}
	}
	return true;
}

// ------------------------------------------------------------------ playing

static void solo_start_question(void)
{
	S.selAns = -1;
	S.typed[0] = '\0';
	S.revealing = false;
	S.lastCorrect = -1;
	S.qStart = osGetTime();
	SoloQ* q = &S.qs[S.qi];
	if (q->qtype == 2) {
		// Slider: start centred.
		S.sVal = (q->sMin + q->sMax) / 2.0f;
	}
	if (q->qtype == 3) {
		// Puzzle: shuffle the order so the correct one isn't showing.
		for (int i = 0; i < q->nAnswers; i++) q->pOrder[i] = i;
		for (int i = q->nAnswers - 1; i > 0; i--) {
			int j = rand() % (i + 1);
			int tmp = q->pOrder[i];
			q->pOrder[i] = q->pOrder[j];
			q->pOrder[j] = tmp;
		}
	}
	if (q->imgUrl[0]) img_request(q->imgUrl);
	else img_request(NULL);
}

static void solo_submit(void)
{
	SoloQ* q = &S.qs[S.qi];
	float secs = (float)(osGetTime() - S.qStart) / 1000.0f;
	if (secs < 0.0f) secs = 0.0f;
	S.lastGain = (int)(1000.0f - secs * 50.0f);
	if (S.lastGain < 0) S.lastGain = 0;

	bool right = false;
	if (q->qtype == 0) {
		if (S.selAns >= 0 && S.selAns < q->nAnswers) right = q->correct[S.selAns] != 0;
	} else if (q->qtype == 2) {
		float d = S.sVal - q->sAnswer;
		if (d < 0.0f) d = -d;
		right = d <= q->sTol;
	} else if (q->qtype == 3) {
		right = true;
		for (int i = 0; i < q->nAnswers; i++) {
			if (q->pOrder[i] != i) { right = false; break; }
		}
	} else {
		right = strcasecmp(S.typed, q->typeAnswer) == 0;
	}
	S.lastCorrect = right ? 1 : 0;
	if (right) S.score += S.lastGain;
	S.revealing = true;
}

// The active list count (filtered or full) and the real quiz index for a
// position in the list.
static int solo_count(void)
{
	return S.search[0] ? S.nFidx : S.nQuizzes;
}

static int solo_real(int pos)
{
	return (S.search[0] && pos >= 0 && pos < S.nFidx) ? S.fidx[pos] : pos;
}

// Rebuilds the search filter (case-insensitive substring match).
static void solo_rebuild_filter(void)
{
	S.nFidx = 0;
	if (!S.search[0]) return;
	for (int i = 0; i < S.nQuizzes; i++) {
		if (strcasestr(S.quizzes[i].name, S.search)) {
			if (S.nFidx < SOLO_MAX_QUIZZES) S.fidx[S.nFidx++] = i;
		}
	}
}

static bool solo_keyboard(const char* hint, char* out, int outsz)
{
	SwkbdState sw;
	swkbdInit(&sw, SWKBD_TYPE_NORMAL, 2, SOLO_A_LEN - 1);
	swkbdSetHintText(&sw, hint);
	if (S.typed[0]) swkbdSetInitialText(&sw, S.typed);
	swkbdSetValidation(&sw, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	char tmp[SOLO_A_LEN];
	if (swkbdInputText(&sw, tmp, sizeof(tmp)) != SWKBD_BUTTON_CONFIRM) return false;
	snprintf(out, outsz, "%s", tmp);
	return true;
}

// Search-box keyboard: empty input is allowed (clears the filter), and the
// left button is "Clear".
static bool solo_keyboard_any(const char* hint, char* out, int outsz)
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

// ------------------------------------------------------------------ stepping

void solo_step(void)
{
	if (!S.active) return;
	if (S.phase == 5) {
		precache_step();
		if (precache_wants_back()) {
			precache_end();
			S.phase = 1;
		}
		return;
	}
	if (S.phase == 1) {
		solo_rebuild_filter();
		thumb_step(); // banner thumbnails load in the list
	}

	if (S.phase == 0 || S.phase == 2) {
		int r = fetch_poll();
		if (r == 0) return;
		if (r < 0) {
			// Retry quietly every frame; the spinner keeps moving.
			if (!S.hget) fetch_begin(S.fetchPath);
			return;
		}
		if (S.phase == 0) {
			if (!solo_parse_list()) {
				solo_error("Could not load the quiz list");
				return;
			}
			S.phase = 1;
		} else {
			if (!solo_parse_questions()) {
				// Surface the server's own error message when there is one.
				static JsDoc edoc;
				char reason[160];
				snprintf(reason, sizeof(reason), "Could not load that quiz");
				if (json_parse(&edoc, S.buf)) {
					const char* e = json_ostr(&edoc, 0, "error", "");
					if (e[0]) {
						snprintf(reason, sizeof(reason), "Could not load that quiz: %s", e);
					} else if (edoc.nodes[0].type == JS_ARR && json_arr_len(&edoc, 0) == 0) {
						snprintf(reason, sizeof(reason),
						         "Could not load that quiz: it has no questions");
					}
				}
				solo_error(reason);
				return;
			}
			S.qi = 0;
			S.score = 0;
			S.phase = 3;
			solo_start_question();
		}
	}
}

// ------------------------------------------------------------------ input

static void solo_input_browse(u32 kDown, touchPosition* touch, bool touched)
{
	if (kDown & KEY_B) {
		S.wantExit = true;
		return;
	}
	int count = solo_count();

	// Y = search the list by name (empty clears the filter).
	if (kDown & KEY_Y) {
		char tmp[96];
		snprintf(tmp, sizeof(tmp), "%s", S.search);
		if (solo_keyboard_any("Search quizzes", tmp, sizeof(tmp))) {
			snprintf(S.search, sizeof(S.search), "%s", tmp);
			S.sel = 0;
			S.scroll = 0;
			solo_rebuild_filter();
		}
		return;
	}

	// Holding Up/Down for 2 seconds starts a fast scroll that only stops
	// when the button is released.
	u32 held = hidKeysHeld();
	bool navHeld = (held & (KEY_UP | KEY_DOWN)) != 0;
	if (navHeld) {
		if (!S.heldNav) {
			S.heldNav = true;
			S.holdAt = osGetTime();
			S.fast = false;
		}
		if (!S.fast && osGetTime() - S.holdAt >= 2000) S.fast = true;
		if (S.fast && osGetTime() >= S.nextRepeat) {
			S.nextRepeat = osGetTime() + 90;
			if (held & KEY_UP) S.sel = S.sel >= 2 ? S.sel - 2 : 0;
			if ((held & KEY_DOWN) && S.sel + 1 < count) {
				S.sel = S.sel + 2 < count ? S.sel + 2 : count - 1;
			}
		}
	} else if (S.heldNav) {
		S.heldNav = false;
		S.fast = false;
	}

	// Taps: Up/Down move one row, Left/Right move one tile (so both columns
	// are reachable).
	if (kDown & KEY_UP) S.sel = S.sel >= 2 ? S.sel - 2 : 0;
	else if ((kDown & KEY_DOWN) && S.sel + 1 < count) {
		S.sel = S.sel + 2 < count ? S.sel + 2 : count - 1;
	}
	if ((kDown & KEY_LEFT) && S.sel > 0) S.sel--;
	if ((kDown & KEY_RIGHT) && S.sel + 1 < count) S.sel++;

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
		if (dir != S.cpDir) {
			S.cpDir = dir;
			S.cpRepeatAt = osGetTime(); // move immediately
		}
		if (dir && osGetTime() >= S.cpRepeatAt) {
			S.cpRepeatAt = osGetTime() + 150;
			if (dir == 1 && S.sel > 0) S.sel--;
			if (dir == 2 && S.sel + 1 < count) S.sel++;
			if (dir == 3) S.sel = S.sel >= 2 ? S.sel - 2 : 0;
			if (dir == 4 && S.sel + 1 < count) {
				S.sel = S.sel + 2 < count ? S.sel + 2 : count - 1;
			}
		}
	}

	// Keep the selection inside the visible 2x3 tile window.
	if (S.sel < S.scroll) S.scroll = (S.sel / SOLO_COLS) * SOLO_COLS;
	if (S.sel >= S.scroll + SOLO_COLS * SOLO_ROWS) {
		S.scroll = ((S.sel - SOLO_COLS * (SOLO_ROWS - 1)) / SOLO_COLS) * SOLO_COLS;
	}

	bool open = false;
	if (touched) {
		int tx = touch->px, ty = touch->py;
		float pageH = SOLO_ROWS * (SOLO_TILE_H + SOLO_GAPY);
		if (ty >= 26 && ty < 26 + pageH) {
			int row = (int)((ty - 26) / (SOLO_TILE_H + SOLO_GAPY));
			int col = (tx < 10 + SOLO_TILE_W + SOLO_GAPX / 2) ? 0 : 1;
			int pos = S.scroll + row * SOLO_COLS + col;
			if (pos >= 0 && pos < count) {
				S.sel = pos;
				open = true;
			}
		}
	}
	if (kDown & KEY_A) open = true;
	// X+SELECT (SELECT pressed while X is held): cache EVERY quiz. Armed on
	// SELECT release so key auto-repeat cannot restart it every 400ms.
	if (hidKeysUp() & KEY_SELECT) S.comboArm = true;
	if (S.comboArm && (hidKeysHeld() & KEY_X) && (kDown & KEY_SELECT)) {
		S.comboArm = false;
		S.phase = 5;
		precache_begin_all(S.host, S.port);
		return;
	}
	if (kDown & KEY_X) {
		// Pre-cache every photo/GIF in this quiz onto the SD card.
		S.phase = 5;
		precache_begin(S.host, S.port, S.quizzes[solo_real(S.sel)].name);
		return;
	}
	if (!open) return;

	if (s_offline) {
		solo_open_offline();
		return;
	}

	// Fetch the chosen quiz.
	char enc[SOLO_NAME_LEN * 4];
	url_encode(enc, sizeof(enc), S.quizzes[solo_real(S.sel)].name);
	char path[SOLO_NAME_LEN * 4 + 16];
	snprintf(path, sizeof(path), "/api/quiz/%s", enc);
	S.phase = 2;
	fetch_begin(path);
}

static void solo_input_play(u32 kDown, touchPosition* touch, bool touched)
{
	if (kDown & KEY_B) {
		// Back to the quiz list.
		S.phase = 1;
		img_request(NULL);
		return;
	}

	if (S.revealing) {
		if (touched || (kDown & KEY_A)) {
			S.qi++;
			if (S.qi >= S.nQ) {
				S.phase = 4;
			} else {
				solo_start_question();
			}
		}
		return;
	}

	SoloQ* q = &S.qs[S.qi];
	if (q->qtype == 0) {
		if (touched) {
			int tx = touch->px, ty = touch->py;
			for (int i = 0; i < q->nAnswers; i++) {
				// Recompute the tile rects the same way draw does.
				float top = 8.0f, bottom = 186.0f;
				float h = (bottom - top - (q->nAnswers - 1) * 8.0f) / q->nAnswers;
				if (h > 56.0f) h = 56.0f;
				float x = 10, y = top + i * (h + 8.0f), w = BOT_W - 20;
				if (q->nAnswers > 3) {
					int r = i / 2, c = i % 2;
					h = (bottom - top - (r + 1 - 1) * 7.0f) / ((q->nAnswers + 1) / 2);
					if (h > 52.0f) h = 52.0f;
					x = 10 + c * 155.0f;
					y = top + r * (h + 7.0f);
					w = 145;
				}
				if (ui_hit(x, y, w, h, tx, ty)) {
					S.selAns = i;
					break;
				}
			}
		}
		if (kDown & (KEY_DOWN | KEY_RIGHT)) {
			S.selAns = (S.selAns + 1) % (q->nAnswers > 0 ? q->nAnswers : 1);
		}
		if (kDown & (KEY_UP | KEY_LEFT)) {
			S.selAns = (S.selAns <= 0 ? q->nAnswers : S.selAns) - 1;
		}

		if ((kDown & KEY_A) || (kDown & KEY_X) ||
		    (touched && ui_hit(10, 194, BOT_W - 20, 38, touch->px, touch->py))) {
			solo_submit();
		}
	} else if (q->qtype == 2) {
		// Slider: drag on the track, D-pad fine steps.
		if (touched && ui_hit(30, 112, BOT_W - 60, 36, touch->px, touch->py)) {
			float t = (float)(touch->px - 30) / (BOT_W - 60);
			if (t < 0.0f) t = 0.0f;
			if (t > 1.0f) t = 1.0f;
			S.sVal = q->sMin + t * (q->sMax - q->sMin);
		}
		float step = (q->sMax - q->sMin) / 100.0f;
		if (step < 0.01f) step = 0.01f;
		if (kDown & KEY_RIGHT) S.sVal += step;
		if (kDown & KEY_LEFT)  S.sVal -= step;
		if (S.sVal < q->sMin) S.sVal = q->sMin;
		if (S.sVal > q->sMax) S.sVal = q->sMax;
		if ((kDown & KEY_A) || (kDown & KEY_X) ||
		    (touched && ui_hit(10, 194, BOT_W - 20, 38, touch->px, touch->py))) {
			solo_submit();
		}
	} else if (q->qtype == 3) {
		// Puzzle: arrow buttons and D-pad moves.
		float gap = 5.0f, top = 14.0f;
		float rowH = (170.0f - (q->nAnswers - 1) * gap) / q->nAnswers;
		if (rowH > 34.0f) rowH = 34.0f;
		if (touched) {
			int tx = touch->px, ty = touch->py;
			for (int i = 0; i < q->nAnswers; i++) {
				float y = top + i * (rowH + gap);
				if (ui_hit(16, y, 34, rowH, tx, ty) && i > 0) {
					int tmp = q->pOrder[i];
					q->pOrder[i] = q->pOrder[i - 1];
					q->pOrder[i - 1] = tmp;
					S.selAns = i - 1;
					break;
				}
				if (ui_hit(BOT_W - 50, y, 34, rowH, tx, ty) && i + 1 < q->nAnswers) {
					int tmp = q->pOrder[i];
					q->pOrder[i] = q->pOrder[i + 1];
					q->pOrder[i + 1] = tmp;
					S.selAns = i + 1;
					break;
				}
			}
		}
		if (kDown & KEY_DOWN && S.selAns + 1 < q->nAnswers) S.selAns++;
		if (kDown & KEY_UP && S.selAns > 0) S.selAns--;
		if (kDown & KEY_RIGHT && S.selAns + 1 < q->nAnswers) {
			int i = S.selAns;
			int tmp = q->pOrder[i];
			q->pOrder[i] = q->pOrder[i + 1];
			q->pOrder[i + 1] = tmp;
			S.selAns++;
		}
		if (kDown & KEY_LEFT && S.selAns > 0) {
			int i = S.selAns;
			int tmp = q->pOrder[i];
			q->pOrder[i] = q->pOrder[i - 1];
			q->pOrder[i - 1] = tmp;
			S.selAns--;
		}
		if ((kDown & KEY_A) || (kDown & KEY_X) ||
		    (touched && ui_hit(10, 194, BOT_W - 20, 38, touch->px, touch->py))) {
			solo_submit();
		}
	} else {
		if ((kDown & KEY_A) ||
		    (touched && ui_hit(16, 46, BOT_W - 32, 92, touch->px, touch->py))) {
			if (solo_keyboard("Type your answer", S.typed, sizeof(S.typed))) {
				solo_submit();
			}
		}
		if ((kDown & KEY_X) && S.typed[0]) solo_submit();
	}
}

static void solo_input_done(u32 kDown, touchPosition* touch, bool touched)
{
	if (kDown & KEY_B) {
		S.wantExit = true;
		return;
	}
	if (touched || (kDown & KEY_A)) {
		S.phase = 1; // back to the list
	}
}

void solo_input(u32 kDown, touchPosition* touch, bool touched)
{
	if (!S.active) return;
	switch (S.phase) {
	case 0:
	case 2:
		if (kDown & KEY_B) S.wantExit = true;
		break;
	case 1: solo_input_browse(kDown, touch, touched); break;
	case 3: solo_input_play(kDown, touch, touched);  break;
	case 4: solo_input_done(kDown, touch, touched);  break;
	case 5: precache_input(kDown, touch, touched);   break;
	}
}

// ------------------------------------------------------------------ drawing

static void solo_draw_spinner(const char* text)
{
	ui_text_center_shadow(text, TOP_W / 2, 84, 0.8f, CLR_WHITE);
	float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
	ui_bar(120, 130, TOP_W - 240, 6, pct, CLR_WHITE, RGBA(255, 255, 255, 60));
	ui_text_center("Press B to cancel", TOP_W / 2, 150, 0.45f, CLR_DIM);
}

void solo_draw_top(void)
{
	char buf[200];
	char lines[UI_MAX_LINES][UI_LINE_MAX];

	switch (S.phase) {
	case 0:
		solo_draw_spinner("Loading quizzes...");
		break;

	case 2:
		solo_draw_spinner("Loading quiz...");
		break;

	case 1: {
		ui_text_center_shadow("SOLO QUIZ", TOP_W / 2, 44, 1.0f, CLR_WHITE);
		if (S.search[0]) {
			snprintf(buf, sizeof(buf), "%d of %d quizzes match \"%s\"",
			         S.nFidx, S.nQuizzes, S.search);
			ui_text_center(buf, TOP_W / 2, 92, 0.45f, CLR_SOFT);
		} else {
			snprintf(buf, sizeof(buf), "%d quizzes on the server", S.nQuizzes);
			ui_text_center(buf, TOP_W / 2, 92, 0.5f, CLR_SOFT);
		}

		snprintf(buf, sizeof(buf), "Selected: %s", S.quizzes[solo_real(S.sel)].name);
		float scale = 0.55f;
		int n = ui_wrap_fit(buf, TOP_W - 60, 70, 0.6f, 0.36f, lines, 3, &scale);
		for (int i = 0; i < n; i++) {
			ui_text_center(lines[i], TOP_W / 2, 118 + i * ui_line_height(scale),
			               scale, CLR_GOLD);
		}
		ui_text_center("Circle Pad / D-pad = move   Hold Up/Down = fast scroll", TOP_W / 2, 200, 0.45f, CLR_DIM);
		ui_text_center("A = play  Y = search  X = pre-cache  B = back", TOP_W / 2, 220, 0.4f, CLR_DIM);
		break;
	}

	case 3: {
		SoloQ* q = &S.qs[S.qi];
		snprintf(buf, sizeof(buf), "Q%d/%d  SOLO", S.qi + 1, S.nQ);
		ui_panel(0, 0, TOP_W, 26, RGBA(40, 8, 60, 80));
		ui_text(buf, 10, 4, 0.52f, CLR_SOFT);
		char scoreBuf[48];
		snprintf(scoreBuf, sizeof(scoreBuf), "%d pts", S.score);
		ui_text_right(scoreBuf, TOP_W - 10, 4, 0.52f, CLR_WHITE);

		bool hasImg = (q->imgUrl[0] != '\0');
		float qh = hasImg ? 88.0f : 128.0f;
		float scale = 0.8f;
		int n = ui_wrap_fit(q->question, TOP_W - 48, qh, 0.85f, 0.42f,
		                    lines, UI_MAX_LINES, &scale);
		float lh = ui_line_height(scale);
		float y = 38 + (qh - n * lh) / 2;
		for (int i = 0; i < n; i++) {
			ui_text_center_shadow(lines[i], TOP_W / 2, y + i * lh, scale, CLR_WHITE);
		}

		if (hasImg) {
			if (img_is_loaded()) {
				img_draw_at(TOP_W / 2, 128, 256, 72);
			} else if (img_pending()) {
				ui_text_center(img_is_from_cache() ? "LOADING FROM CACHE"
				               : (img_is_gif() ? "DOWNLOADING GIF" : "DOWNLOADING IMAGE"),
				               TOP_W / 2, 150, 0.42f, CLR_SOFT);
				float p = img_progress();
				if (p >= 0.0f) {
					ui_bar(120, 166, TOP_W - 240, 10, p, CLR_WHITE,
					       RGBA(255, 255, 255, 60));
					snprintf(buf, sizeof(buf), "%d%%", (int)(p * 100.0f));
					ui_text_center(buf, TOP_W / 2, 184, 0.45f, CLR_SOFT);
				} else {
					float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
					ui_bar(120, 166, TOP_W - 240, 10, pct, CLR_WHITE,
					       RGBA(255, 255, 255, 60));
					ui_text_center("connecting...", TOP_W / 2, 184, 0.42f, CLR_DIM);
				}
			}
		}

		if (!S.revealing && !q->isMC) {
			ui_text_center("Type your answer on the touch screen", TOP_W / 2, 206, 0.45f, CLR_DIM);
		} else if (!S.revealing) {
			float secs = (float)(osGetTime() - S.qStart) / 1000.0f;
			float pct2 = 1.0f - secs / 20.0f;
			if (pct2 < 0.0f) pct2 = 0.0f;
			int pts = (int)(1000.0f - secs * 50.0f);
			if (pts < 0) pts = 0;
			ui_bar(40, 208, TOP_W - 80, 8, pct2,
			       ui_lerp(CLR_BAD, CLR_GOOD, pct2), RGBA(255, 255, 255, 55));
			snprintf(buf, sizeof(buf), "%d points available", pts);
			ui_text_center(buf, TOP_W / 2, 220, 0.4f, CLR_SOFT);
		}
		break;
	}

	case 5:
		precache_draw_top();
		break;

	case 4: {
		ui_text_center_shadow("QUIZ COMPLETE", TOP_W / 2, 60, 0.9f, CLR_WHITE);
		snprintf(buf, sizeof(buf), "You scored %d points", S.score);
		ui_text_center(buf, TOP_W / 2, 110, 0.6f, CLR_GOLD);
		int maxPts = S.nQ * 1000;
		snprintf(buf, sizeof(buf), "out of a possible %d", maxPts);
		ui_text_center(buf, TOP_W / 2, 136, 0.45f, CLR_SOFT);
		ui_text_center("A = back to the list, B = setup", TOP_W / 2, 200, 0.45f, CLR_DIM);
		break;
	}
	}
}

void solo_draw_bottom(void)
{
	char buf[200];
	char lines[UI_MAX_LINES][UI_LINE_MAX];

	switch (S.phase) {
	case 0:
		ui_text_center("Fetching the quiz list...", BOT_W / 2, 110, 0.5f, CLR_SOFT);
		break;

	case 2:
		ui_text_center("Fetching questions...", BOT_W / 2, 110, 0.5f, CLR_SOFT);
		break;

	case 1: {
		int count = solo_count();
		ui_text("QUIZZES", 14, 8, 0.5f, CLR_SOFT);
		ui_text_right("A = play  Y = search", BOT_W - 14, 10, 0.38f, CLR_DIM);
		for (int v = 0; v < SOLO_COLS * SOLO_ROWS; v++) {
			int pos = S.scroll + v;
			if (pos >= count) break;
			int idx = solo_real(pos);
			int r = v / SOLO_COLS, c = v % SOLO_COLS;
			float x = 10 + c * (SOLO_TILE_W + SOLO_GAPX);
			float y = 26 + r * (SOLO_TILE_H + SOLO_GAPY);
			bool sel = (pos == S.sel);
			if (sel) {
				C2D_DrawRectSolid(x - 2 + ui_parallax(PAR_PANEL), y - 2, 0.08f,
				                  SOLO_TILE_W + 4, SOLO_TILE_H + 4, CLR_WHITE);
			}
			u32 fill = sel ? ui_fade(CLR_GOLD, 0.30f) : CLR_GLASS;
			ui_tile(x, y, SOLO_TILE_W, SOLO_TILE_H, fill, false);

			// Banner photo on top: a down-arrow while it downloads, or a
			// white ? on dark gray when the quiz has no banner at all.
			if (S.quizzes[idx].banner) {
				if (s_offline) {
					char cp[420];
					snprintf(cp, sizeof(cp),
					         "sdmc:/QuizFabric/cachedfiles/%s/banner.565",
					         S.quizzes[idx].name);
					thumb_load_file(idx, cp);
				} else {
					char enc[SOLO_NAME_LEN * 4];
					url_encode(enc, sizeof(enc), S.quizzes[idx].name);
					char path[SOLO_NAME_LEN * 4 + 24];
					snprintf(path, sizeof(path), "/api/quizbanner/%s", enc);
					thumb_request(idx, path);
				}
			}
			thumb_draw(idx, x + 4, y + 4, SOLO_TILE_W - 8, 30,
			           S.quizzes[idx].banner);

			float sc = 0.42f;
			int nl = ui_wrap_fit(S.quizzes[idx].name, SOLO_TILE_W - 12, 18,
			                      0.42f, 0.26f, lines, 2, &sc);
			for (int k = 0; k < nl; k++) {
				ui_text_center(lines[k], x + SOLO_TILE_W / 2,
				               y + 36 + k * ui_line_height(sc), sc,
				               sel ? CLR_GOLD : CLR_WHITE);
			}
		}
		if (count == 0) {
			ui_text_center(S.search[0] ? "No quizzes match" : "No quizzes found",
			               BOT_W / 2, 110, 0.5f, CLR_DIM);
		}
		break;
	}

	case 3: {
		SoloQ* q = &S.qs[S.qi];
		if (S.revealing) {
			u32 accent = S.lastCorrect ? CLR_GOOD : CLR_BAD;
			ui_text_center(S.lastCorrect ? "CORRECT!" : "NOT QUITE", BOT_W / 2, 40,
			               0.85f, accent);
			if (S.lastCorrect && S.lastGain > 0) {
				snprintf(buf, sizeof(buf), "+%d points", S.lastGain);
				ui_text_center(buf, BOT_W / 2, 76, 0.6f, CLR_GOLD);
			}
			// Show the correct answer(s).
			char corr[SOLO_A_LEN * SOLO_MAX_ANSWERS];
			corr[0] = '\0';
			const char* ca = q->typeAnswer;
			if (q->qtype == 0) {
				for (int i = 0; i < q->nAnswers; i++) {
					if (q->correct[i]) {
						if (corr[0]) strncat(corr, ", ", sizeof(corr) - strlen(corr) - 1);
						strncat(corr, q->answers[i], sizeof(corr) - strlen(corr) - 1);
					}
				}
				ca = corr;
			} else if (q->qtype == 2) {
				snprintf(corr, sizeof(corr), "%.1f", q->sAnswer);
				ca = corr;
			} else if (q->qtype == 3) {
				for (int i = 0; i < q->nAnswers; i++) {
					if (corr[0]) strncat(corr, " -> ", sizeof(corr) - strlen(corr) - 1);
					strncat(corr, q->answers[i], sizeof(corr) - strlen(corr) - 1);
				}
				ca = corr;
			}
			ui_text_center("ANSWER", BOT_W / 2, 104, 0.42f, CLR_DIM);
			float scale = 0.6f;
			int n = ui_wrap_fit(ca, BOT_W - 52, 50, 0.65f, 0.34f, lines, 3, &scale);
			for (int i = 0; i < n; i++) {
				ui_text_center(lines[i], BOT_W / 2, 122 + i * ui_line_height(scale),
				               scale, CLR_GOLD);
			}
			ui_tile(10, 194, BOT_W - 20, 38, RGB(56, 214, 160), false);
			ui_text_center(S.qi + 1 >= S.nQ ? "FINISH" : "NEXT",
			               BOT_W / 2, 206, 0.6f, CLR_INK);
			return;
		}

		if (q->qtype == 0) {
			float top = 8.0f, bottom = 186.0f;
			int na = q->nAnswers;
			for (int i = 0; i < na; i++) {
				float h, x, y, w;
				if (na <= 3) {
					h = (bottom - top - (na - 1) * 8.0f) / na;
					if (h > 56.0f) h = 56.0f;
					x = 10; y = top + i * (h + 8.0f); w = BOT_W - 20;
				} else {
					int rows = (na + 1) / 2;
					h = (bottom - top - (rows - 1) * 7.0f) / rows;
					if (h > 52.0f) h = 52.0f;
					int r = i / 2, c = i % 2;
					x = 10 + c * 155.0f; y = top + r * (h + 7.0f); w = 145;
				}
				bool sel = (S.selAns == i);
				if (sel) {
					C2D_DrawRectSolid(x - 3 + ui_parallax(PAR_PANEL), y - 3, 0.08f, w + 6, h + 6, CLR_WHITE);
				}
				ui_tile(x, y, w, h, ui_answer_color(i), sel);
				float scale = 0.55f;
				int n = ui_wrap_fit(q->answers[i], w - 16, h - 10,
				                    0.58f, 0.32f, lines, 3, &scale);
				float lh = ui_line_height(scale);
				float ty = y + (h - n * lh) / 2;
				for (int k = 0; k < n; k++) {
					ui_text_center(lines[k], x + w / 2, ty + k * lh, scale, CLR_WHITE);
				}
			}
		} else if (q->qtype == 2) {
			// Slider: track, knob, live value.
			ui_text("DRAG THE SLIDER", 18, 26, 0.45f, CLR_DIM);
			ui_bar(30, 112 + 36 / 2 - 5, BOT_W - 60, 10, 1.0f,
			       RGBA(255, 255, 255, 60), RGBA(255, 255, 255, 25));
			float t = (S.sVal - q->sMin) / (q->sMax - q->sMin);
			C2D_DrawCircleSolid(30 + t * (BOT_W - 60), 112 + 36 / 2, 0.5f,
			                    14, CLR_GOLD);
			snprintf(buf, sizeof(buf), "%.1f", S.sVal);
			ui_text_center_shadow(buf, BOT_W / 2, 58, 1.0f, CLR_GOLD);
			snprintf(buf, sizeof(buf), "%.0f", q->sMin);
			ui_text(buf, 30, 170, 0.42f, CLR_DIM);
			snprintf(buf, sizeof(buf), "%.0f", q->sMax);
			ui_text_right(buf, 30 + BOT_W - 60, 170, 0.42f, CLR_DIM);
		} else if (q->qtype == 3) {
			// Puzzle rows with up/down arrows.
			ui_text("PUT THEM IN ORDER", 18, 6, 0.42f, CLR_DIM);
			float gap = 5.0f, top = 14.0f;
			float rowH = (170.0f - (q->nAnswers - 1) * gap) / q->nAnswers;
			if (rowH > 34.0f) rowH = 34.0f;
			for (int i = 0; i < q->nAnswers; i++) {
				float y = top + i * (rowH + gap);
				bool sel = (S.selAns == i);
				u32 fill = sel ? ui_fade(CLR_GOLD, 0.30f) : CLR_GLASS;
				ui_panel_hi(56, y, BOT_W - 112, rowH, fill);
				snprintf(buf, sizeof(buf), "%d.", i + 1);
				ui_text(buf, 64, y + rowH / 2 - 8, 0.4f, CLR_DIM);
				float scale = 0.5f;
				int n = ui_wrap_fit(q->answers[q->pOrder[i]], BOT_W - 152, rowH - 8,
				                    0.52f, 0.3f, lines, 2, &scale);
				float lh = ui_line_height(scale);
				float ty = y + (rowH - n * lh) / 2;
				for (int k = 0; k < n; k++) {
					ui_text(lines[k], 88, ty + k * lh, scale,
					        sel ? CLR_GOLD : CLR_WHITE);
				}
				u32 uf = (i > 0) ? RGBA(255, 255, 255, 90) : RGBA(255, 255, 255, 30);
				ui_tile(16, y, 34, rowH, uf, false);
				ui_text_center("^", 33, y + rowH / 2 - 9, 0.55f, CLR_WHITE);
				u32 df = (i + 1 < q->nAnswers) ? RGBA(255, 255, 255, 90) : RGBA(255, 255, 255, 30);
				ui_tile(BOT_W - 50, y, 34, rowH, df, false);
				ui_text_center("v", BOT_W - 33, y + rowH / 2 - 9, 0.55f, CLR_WHITE);
			}
		} else {
			ui_text("YOUR ANSWER", 18, 24, 0.45f, CLR_DIM);
			ui_panel_hi(16, 46, BOT_W - 32, 92, CLR_GLASS);
			if (S.typed[0]) {
				float scale = 0.65f;
				int n = ui_wrap_fit(S.typed, BOT_W - 52, 76, 0.7f, 0.35f, lines, 4, &scale);
				float lh = ui_line_height(scale);
				float ty = 46 + (92 - n * lh) / 2;
				for (int k = 0; k < n; k++) {
					ui_text_center(lines[k], BOT_W / 2, ty + k * lh, scale, CLR_WHITE);
				}
			} else {
				ui_text_center("tap to type", BOT_W / 2, 84, 0.55f, CLR_DIM);
			}
		}

		bool can = (q->qtype != 0 || S.selAns >= 0) && (q->qtype != 1 || S.typed[0]);
		u32 fill = can ? RGB(56, 214, 160) : RGBA(255, 255, 255, 55);
		ui_tile(10, 194, BOT_W - 20, 38, fill, false);
		const char* label = can ? "SUBMIT" : (q->qtype == 0 ? "PICK AN ANSWER" : "TYPE AN ANSWER");
		ui_text_center(label, BOT_W / 2, 206, 0.58f, can ? CLR_INK : CLR_DIM);
		break;
	}

	case 5:
		precache_draw_bottom();
		break;

	case 4: {
		snprintf(buf, sizeof(buf), "%d points", S.score);
		ui_text_center_shadow(buf, BOT_W / 2, 80, 0.9f, CLR_GOLD);
		ui_tile(60, 120, BOT_W - 120, 40, RGB(56, 214, 160), false);
		ui_text_center("PLAY AGAIN / LIST", BOT_W / 2, 132, 0.55f, CLR_INK);
		ui_text_center("B = back to setup", BOT_W / 2, 190, 0.42f, CLR_DIM);
		break;
	}
	}
}
