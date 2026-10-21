// Pre-cache quiz photos and GIFs onto the SD card. See precache.h.
//
// X on a quiz in the solo/host lists caches that quiz; X+SELECT on the
// cache screen deletes EVERYTHING cached and re-downloads every quiz's
// images from scratch. Files already cached are skipped, interrupted
// downloads resume from their partial file, and 404s are dropped.

#include "precache.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "json.h"
#include "solo.h" // img_cache_paths
#include "thumb.h"
#include "ui.h"
#include "ws.h"

#define PC_MAX_IMAGES 256
#define PC_MAX_QUIZZES 128

typedef struct {
	char url[256];  // percent-encoded .565 path on the server
	char cache[320]; // sdmc cachedfiles path
	char dl[320];    // sdmc downloading path
	int  qIdx;       // 1-based question number (0 for the banner)
	bool isBanner;
} PcItem;

typedef struct {
	bool  active;
	bool  back;
	int   phase;     // 0 fetch quiz list, 1 fetch questions, 2 download,
	                 // 3 done, 4 error
	bool  allMode;
	char  host[80];
	int   port;
	char  quizzes[PC_MAX_QUIZZES][96];
	int   nQz;
	int   qi;        // current quiz index
	char  quiz[96];  // current quiz name
	int   nQuestions;
	int   n;
	PcItem items[PC_MAX_IMAGES];
	int   i;         // current item
	bool  dlRunning;
	int   dlOffset;
	FILE* dlF;
	HttpGet* hget;
	u64   retryAt;
	int   failStreak; // consecutive failed attempts on the current item
	int   skipped;    // files skipped (gone or stuck)
	int   done;       // files cached
	bool  comboArm;   // X+SELECT armed (SELECT must release first)
	u64   phase0At;   // watchdog for the quiz-list fetch
	int   phase0Tries;
	u64   phase1At;   // watchdog for a quiz's question fetch
	char  err[160];
	char  buf[131072];
} Pre;

static Pre P;

static void pc_error(const char* m)
{
	snprintf(P.err, sizeof(P.err), "%s", m);
	P.phase = 4;
}

static void pc_encode(char* dst, int dstsz, const char* src)
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

static void pc_mkdir(const char* path)
{
	mkdir(path, 0777);
}

// Starts fetching the question list of the current quiz (P.quiz, P.qi).
static void pc_fetch_questions(void)
{
	char enc[400];
	pc_encode(enc, sizeof(enc), P.quiz);
	char path[420];
	snprintf(path, sizeof(path), "/api/quiz/%s", enc);
	P.hget = http_get_begin(P.host, P.port, path, 8000);
	if (!P.hget) pc_error("Could not reach the server");
	P.phase = 1;
	P.phase1At = osGetTime();
}

// Parses the questions JSON in P.buf into P.items (unique image URLs only).
static void pc_parse_questions(void)
{
	static JsDoc doc;
	P.n = 0;
	P.nQuestions = 0;
	// Save the questions for offline play FIRST: json_parse rewrites the
	// buffer in place (NULs appear inside it), so strlen() afterwards would
	// truncate the quiz.json file.
	{
		char qdir[400];
		snprintf(qdir, sizeof(qdir), "sdmc:/QuizFabric/cachedfiles/%s", P.quiz);
		pc_mkdir("sdmc:/QuizFabric");
		pc_mkdir("sdmc:/QuizFabric/cachedfiles");
		pc_mkdir(qdir);
		char qj[420];
		snprintf(qj, sizeof(qj), "%s/quiz.json", qdir);
		FILE* qf = fopen(qj, "wb");
		if (qf) {
			fwrite(P.buf, 1, strlen(P.buf), qf);
			fclose(qf);
		}
	}
	if (!json_parse(&doc, P.buf)) {
		pc_error("The server sent a bad reply");
		return;
	}
	int nq = json_arr_len(&doc, 0);
	P.nQuestions = nq;
	for (int q = 0; q < nq && P.n < PC_MAX_IMAGES; q++) {
		int qn = json_arr_at(&doc, 0, q);
		int imgs = json_obj_get(&doc, qn, "images");
		int ni = json_arr_len(&doc, imgs);
		for (int k = 0; k < ni; k++) {
			const char* u = json_ostr(&doc, json_arr_at(&doc, imgs, k),
			                          "url", "");
			if (!u[0]) continue;
			char t[256];
			snprintf(t, sizeof(t), "%s", u);
			char* dot = strrchr(t, '.');
			if (dot) {
				*dot = '\0';
				snprintf(dot, sizeof(t) - (size_t)(dot - t), ".565");
			}
			char quizB[96], cacheB[320], dlB[320];
			if (!img_cache_paths(t, quizB, sizeof(quizB),
			                     cacheB, sizeof(cacheB), dlB, sizeof(dlB))) continue;
			if (strcmp(quizB, P.quiz) != 0) continue;
			char enc[256];
			pc_encode(enc, sizeof(enc), t);
			bool dup = false;
			for (int m = 0; m < P.n; m++) {
				if (!strcmp(P.items[m].url, enc)) {
					dup = true;
					break;
				}
			}
			if (dup) continue;
			snprintf(P.items[P.n].url, sizeof(P.items[0].url), "%s", enc);
			snprintf(P.items[P.n].cache, sizeof(P.items[0].cache), "%s", cacheB);
			snprintf(P.items[P.n].dl, sizeof(P.items[0].dl), "%s", dlB);
			P.items[P.n].qIdx = q + 1;
			P.items[P.n].isBanner = false;
			P.n++;
		}
	}
	// Also cache the banner thumbnail (the list picture). A missing banner
	// just 404s and gets skipped, so this is always safe to try.
	if (P.n < PC_MAX_IMAGES) {
		char enc[256];
		char path[280];
		snprintf(path, sizeof(path), "/api/quizbanner/%s", P.quiz);
		pc_encode(enc, sizeof(enc), path);
		snprintf(P.items[P.n].url, sizeof(P.items[0].url), "%s", enc);
		snprintf(P.items[P.n].cache, sizeof(P.items[0].cache),
		         "sdmc:/QuizFabric/cachedfiles/%s/banner.565", P.quiz);
		snprintf(P.items[P.n].dl, sizeof(P.items[0].dl),
		         "sdmc:/QuizFabric/downloading/%s~banner.565", P.quiz);
		P.items[P.n].qIdx = 0;
		P.items[P.n].isBanner = true;
		P.n++;
	}
}

// Starts (or resumes) the download of the current item.
static void pc_start_item(void)
{
	PcItem* it = &P.items[P.i];
	pc_mkdir("sdmc:/QuizFabric");
	pc_mkdir("sdmc:/QuizFabric/downloading");
	pc_mkdir("sdmc:/QuizFabric/cachedfiles");
	// The per-quiz folder must exist before rename() can move into it.
	char qdir[400];
	snprintf(qdir, sizeof(qdir), "sdmc:/QuizFabric/cachedfiles/%s", P.quiz);
	pc_mkdir(qdir);
	P.dlF = fopen(it->dl, "r+b");
	if (!P.dlF) P.dlF = fopen(it->dl, "wb");
	if (!P.dlF) {
		P.failStreak++;
		P.retryAt = osGetTime() + 1000;
		return;
	}
	fseek(P.dlF, 0, SEEK_END);
	P.dlOffset = (int)ftell(P.dlF);
	setvbuf(P.dlF, NULL, _IOFBF, 16384);
	P.hget = http_get_begin_file(P.host, P.port, it->url, 20000,
	                             P.dlF, P.dlOffset, it->dl);
	if (!P.hget) {
		fclose(P.dlF);
		P.dlF = NULL;
		P.failStreak++;
		P.retryAt = osGetTime() + 1000;
		return;
	}
	P.dlRunning = true;
}

// Moves to the next not-yet-cached item (or the next quiz, or done).
static void pc_advance(void)
{
	for (;;) {
		if (P.i >= P.n) {
			// This quiz is done: next quiz (all mode) or finished.
			if (P.allMode && P.qi + 1 < P.nQz) {
				P.qi++;
				snprintf(P.quiz, sizeof(P.quiz), "%s", P.quizzes[P.qi]);
				pc_fetch_questions();
			} else {
				P.phase = 3;
			}
			return;
		}
		FILE* cf = fopen(P.items[P.i].cache, "rb");
		if (cf) {
			fclose(cf); // already cached: skip
			P.i++;
			continue;
		}
		pc_start_item();
		return;
	}
}

void precache_begin(const char* host, int port, const char* quizName)
{
	precache_end();
	memset(&P, 0, sizeof(P));
	snprintf(P.host, sizeof(P.host), "%s", host ? host : "");
	P.port = port;
	snprintf(P.quiz, sizeof(P.quiz), "%s", quizName ? quizName : "");
	P.allMode = false;
	P.active = true;
	pc_fetch_questions();
}

// X+SELECT: wipe every cached file and re-cache ALL quizzes from scratch.
void precache_begin_all(const char* host, int port)
{
	precache_end();
	memset(&P, 0, sizeof(P));
	snprintf(P.host, sizeof(P.host), "%s", host ? host : "");
	P.port = port;
	P.allMode = true;
	P.active = true;
	pc_mkdir("sdmc:/QuizFabric");
	// No wiping: existing cache stays; only missing files are downloaded.
	// Drop any in-flight thumbnail download so the SOC is free and quiet.
	thumb_reset();
	P.hget = http_get_begin(P.host, P.port, "/api/quizzes", 30000);
	if (!P.hget) pc_error("Could not reach the server");
	P.phase = 0;
	P.phase0At = osGetTime();
}

bool precache_wants_back(void) { return P.back; }

void precache_end(void)
{
	if (P.hget) {
		http_get_close(P.hget);
		P.hget = NULL;
	}
	if (P.dlF) {
		fclose(P.dlF);
		P.dlF = NULL;
	}
	P.active = false;
	P.back = false;
	P.dlRunning = false;
}

void precache_step(void)
{
	if (!P.active) return;

	if (P.phase == 0) {
		// Watchdog: the list fetch can never spin forever.
		if (osGetTime() - P.phase0At > 45000) {
			if (P.hget) {
				http_get_close(P.hget);
				P.hget = NULL;
			}
			pc_error("Timed out reading the quiz list");
			return;
		}
		if (!P.hget) return;
		int r = http_get_step(P.hget);
		if (r == 0) return;
		int ok = 0;
		if (r == 1) {
			char* b = http_get_take_body(P.hget);
			if (b) {
				snprintf(P.buf, sizeof(P.buf), "%s", b);
				free(b);
				ok = 1;
			}
		}
		http_get_close(P.hget);
		P.hget = NULL;
		if (!ok) {
			if (P.phase0Tries < 5) {
				P.phase0Tries++;
				P.phase0At = osGetTime();
				P.hget = http_get_begin(P.host, P.port, "/api/quizzes", 30000);
				if (!P.hget) pc_error("Could not reach the server");
				return;
			}
			pc_error("Could not load the quiz list");
			return;
		}
		static JsDoc doc;
		if (!json_parse(&doc, P.buf)) {
			pc_error("The server sent a bad reply");
			return;
		}
		int arr = json_obj_get(&doc, 0, "quizzes");
		int n = json_arr_len(&doc, arr);
		P.nQz = 0;
		for (int k = 0; k < n && P.nQz < PC_MAX_QUIZZES; k++) {
			const char* nm = json_ostr(&doc, json_arr_at(&doc, arr, k),
			                           "name", "");
			if (!nm[0]) continue;
			snprintf(P.quizzes[P.nQz], sizeof(P.quizzes[0]), "%s", nm);
			P.nQz++;
		}
		if (P.nQz == 0) {
			P.phase = 3;
			return;
		}
		P.qi = 0;
		snprintf(P.quiz, sizeof(P.quiz), "%s", P.quizzes[0]);
		pc_fetch_questions();
		return;
	}

	if (P.phase == 1) {
		// Watchdog: a question fetch can never spin forever either.
		if (osGetTime() - P.phase1At > 20000) {
			if (P.hget) {
				http_get_close(P.hget);
				P.hget = NULL;
			}
			pc_error("Timed out reading a quiz");
			return;
		}
		if (!P.hget) return;
		int r = http_get_step(P.hget);
		if (r == 0) return;
		int ok = 0;
		if (r == 1) {
			char* b = http_get_take_body(P.hget);
			if (b) {
				snprintf(P.buf, sizeof(P.buf), "%s", b);
				free(b);
				ok = 1;
			}
		}
		http_get_close(P.hget);
		P.hget = NULL;
		if (!ok) {
			pc_error("Could not load a quiz");
			return;
		}
		pc_parse_questions();
		if (P.phase != 4) {
			P.i = 0;
			P.phase = 2;
			pc_advance();
		}
		return;
	}

	if (P.phase == 2) {
		if (P.dlRunning) {
			int r = http_get_step(P.hget);
			if (r == 0) return;
			int status = http_get_status(P.hget);
			int downloaded = http_get_downloaded(P.hget);
			http_get_close(P.hget);
			P.hget = NULL;
			fclose(P.dlF);
			P.dlF = NULL;
			P.dlRunning = false;
			if (status == 404) {
				// Image gone server-side: skip it and drop the partial.
				remove(P.items[P.i].dl);
				P.skipped++;
				P.failStreak = 0;
				P.i++;
				pc_advance();
				return;
			}
			if (r == 1) {
				remove(P.items[P.i].cache);
				rename(P.items[P.i].dl, P.items[P.i].cache);
				P.done++;
				P.failStreak = 0;
				P.i++;
				pc_advance();
				return;
			}
			// Interrupted: the partial stays; resume from its size. Progress
			// resets the failure streak, so one bad image cannot stall the
			// whole run forever.
			if (downloaded > P.dlOffset) P.failStreak = 0;
			else P.failStreak++;
			P.dlOffset = downloaded;
			if (P.failStreak >= 30) {
				remove(P.items[P.i].dl);
				P.skipped++;
				P.failStreak = 0;
				P.i++;
				pc_advance();
				return;
			}
			P.retryAt = osGetTime() + 1000;
			return;
		}
		if (osGetTime() < P.retryAt) return;
		if (P.failStreak >= 30) {
			// Give up on this file so the run can continue.
			remove(P.items[P.i].dl);
			P.skipped++;
			P.failStreak = 0;
			P.i++;
			pc_advance();
			return;
		}
		pc_start_item();
	}
}

void precache_input(u32 kDown, touchPosition* touch, bool touched)
{
	if (!P.active) return;

	// X+SELECT (SELECT pressed while X is held) = cache every quiz (only
	// missing files are downloaded; nothing is wiped). Armed on SELECT
	// release so key auto-repeat cannot restart the run every 400ms.
	if (hidKeysUp() & KEY_SELECT) P.comboArm = true;
	if (P.comboArm && (hidKeysHeld() & KEY_X) && (kDown & KEY_SELECT)) {
		P.comboArm = false;
		precache_begin_all(P.host, P.port);
		return;
	}

	if (P.phase == 3 || P.phase == 4) {
		if ((kDown & KEY_A) || (kDown & KEY_B) || touched) P.back = true;
	} else if (kDown & KEY_B) {
		P.back = true; // cancel mid-download (keeps what finished)
	}
}

void precache_draw_top(void)
{
	char buf[200];
	char lines[UI_MAX_LINES][UI_LINE_MAX];

	switch (P.phase) {
	case 0:
		ui_text_center_shadow("CACHING ALL QUIZZES", TOP_W / 2, 70, 0.8f, CLR_WHITE);
		ui_text_center("Reading the quiz list...", TOP_W / 2, 110, 0.5f, CLR_SOFT);
		{
			float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
			ui_bar(120, 140, TOP_W - 240, 6, pct, CLR_WHITE, RGBA(255, 255, 255, 60));
		}
		break;

	case 1:
	case 2: {
		ui_text_center_shadow(P.allMode ? "CACHING ALL QUIZZES" : "CACHING QUIZ PHOTOS",
		                      TOP_W / 2, 32, 0.7f, CLR_WHITE);

		// Bar 1: which quiz we are on.
		int shownQz = P.allMode ? P.qi + 1 : 1;
		int totQz = P.allMode ? (P.nQz > 0 ? P.nQz : 1) : 1;
		snprintf(buf, sizeof(buf), "QUIZ %d/%d", shownQz, totQz);
		ui_text_center(buf, TOP_W / 2, 58, 0.44f, CLR_SOFT);
		ui_bar(120, 76, TOP_W - 240, 7,
		       P.allMode && P.nQz > 0 ? (float)P.qi / (float)P.nQz : 0.0f,
		       CLR_GOLD, RGBA(255, 255, 255, 60));
		float scale = 0.4f;
		int nl = ui_wrap_fit(P.quiz, TOP_W - 60, 18, 0.42f, 0.28f, lines, 2, &scale);
		for (int k = 0; k < nl; k++) {
			ui_text_center(lines[k], TOP_W / 2, 88 + k * ui_line_height(scale),
			               scale, CLR_DIM);
		}

		// Bar 2: which question we are on (or the banner).
		if (P.i >= 0 && P.i < P.n && P.items[P.i].isBanner) {
			snprintf(buf, sizeof(buf), "BANNER");
			ui_text_center(buf, TOP_W / 2, 116, 0.44f, CLR_SOFT);
			ui_bar(120, 134, TOP_W - 240, 7, 1.0f,
			       CLR_GOLD, RGBA(255, 255, 255, 60));
		} else {
			int shownQ = (P.i >= 0 && P.i < P.n) ? P.items[P.i].qIdx : 0;
			int totQ = P.nQuestions > 0 ? P.nQuestions : 1;
			snprintf(buf, sizeof(buf), "QUESTION %d/%d", shownQ, totQ);
			ui_text_center(buf, TOP_W / 2, 116, 0.44f, CLR_SOFT);
			ui_bar(120, 134, TOP_W - 240, 7,
			       totQ > 1 ? (float)(shownQ - 1) / (float)(totQ - 1) : 1.0f,
			       CLR_GOLD, RGBA(255, 255, 255, 60));
		}

		// Bar 3: the current file, with the percentage UNDER the bar.
		ui_text_center("CURRENT FILE", TOP_W / 2, 152, 0.4f, CLR_DIM);
		float p = (P.dlRunning && P.hget) ? http_get_progress(P.hget) : -1.0f;
		if (p >= 0.0f) {
			if (p > 1.0f) p = 1.0f;
			ui_bar(120, 168, TOP_W - 240, 8, p, CLR_WHITE,
			       RGBA(255, 255, 255, 60));
			int pct = (int)(p * 100.0f);
			snprintf(buf, sizeof(buf), pct < 5 ? "under 5%%" : "%d%%", pct);
			ui_text_center(buf, TOP_W / 2, 182, 0.4f, CLR_SOFT);
		} else {
			float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
			ui_bar(120, 168, TOP_W - 240, 8, pct, CLR_WHITE,
			       RGBA(255, 255, 255, 60));
			ui_text_center("waiting...", TOP_W / 2, 182, 0.4f, CLR_DIM);
		}

		ui_text_center("X+SELECT = cache every quiz (only missing files)", TOP_W / 2, 212,
		               0.36f, CLR_DIM);
		break;
	}

	case 3:
		ui_text_center_shadow("ALL CACHED!", TOP_W / 2, 70, 0.9f, CLR_GOOD);
		snprintf(buf, sizeof(buf), "%d file%s cached on the SD card",
		         P.done, P.done == 1 ? "" : "s");
		ui_text_center(buf, TOP_W / 2, 116, 0.5f, CLR_SOFT);
		if (P.skipped > 0) {
			snprintf(buf, sizeof(buf), "%d skipped (unavailable)", P.skipped);
			ui_text_center(buf, TOP_W / 2, 142, 0.42f, CLR_BAD);
		} else {
			ui_text_center(P.allMode ? "for every quiz" : "for this quiz",
			               TOP_W / 2, 142, 0.42f, CLR_DIM);
		}
		ui_text_center("They now load instantly from cache", TOP_W / 2, 176, 0.45f, CLR_SOFT);
		break;

	case 4:
		ui_text_center_shadow("COULD NOT CACHE", TOP_W / 2, 84, 0.8f, CLR_BAD);
		if (P.err[0]) ui_text_center(P.err, TOP_W / 2, 126, 0.45f, CLR_SOFT);
		break;
	}
}

void precache_draw_bottom(void)
{
	switch (P.phase) {
	case 0:
		ui_text_center("Reading the quiz list...", BOT_W / 2, 110, 0.5f, CLR_SOFT);
		break;
	case 1:
	case 2:
		ui_tile(60, 140, BOT_W - 120, 40, RGBA(255, 92, 116, 220), false);
		ui_text_center("B = CANCEL", BOT_W / 2, 152, 0.55f, CLR_WHITE);
		ui_text_center("What is done so far stays cached", BOT_W / 2, 198, 0.4f, CLR_DIM);
		break;
	case 3:
		ui_tile(60, 140, BOT_W - 120, 44, RGB(56, 214, 160), false);
		ui_text_center("BACK TO THE LIST", BOT_W / 2, 154, 0.55f, CLR_INK);
		break;
	case 4:
		ui_text_center("Press A or B to go back", BOT_W / 2, 110, 0.5f, CLR_DIM);
		break;
	}
}
