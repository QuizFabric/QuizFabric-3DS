// Solo play for the QuizFabric DS client. Mirrors the 3DS solo flow:
// quiz list -> questions (mc / type-in / slider / puzzle) -> reveal ->
// results, with local scoring and back buttons everywhere.
//
// No GIF support by design: question images arrive as tiny .ds bitmaps
// (raw RGB555, low resolution) and GIF URLs show the GIF_MSG notice.

#include "solo.h"

#include "gfx.h"
#include "json.h"
#include "kbd.h"
#include "net.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_QUIZZES  64
#define QUIZ_BUF     131072
#define IMG_BUF      65536
#define MAX_ANSWERS  8
#define ANS_LEN      128
#define QLINE_LEN    96

static const char GIF_MSG[] =
    "Cannot load GIF. DS is not powerful enough to load GIFs. Use a 3DS instead.";

typedef enum {
	PH_LOAD_LIST,   // fetching /api/quizzes
	PH_LIST,        // quiz list
	PH_LOAD_QUIZ,   // fetching /api/quiz/<name>
	PH_QUESTION,    // answering
	PH_REVEAL,      // correct/not + points
	PH_RESULTS,     // final score
	PH_ERROR
} Phase;

typedef struct {
	char name[96];
	bool banner;
} QuizEntry;

static char s_host[64];
static int  s_port;

static Phase s_phase = PH_LOAD_LIST;
static char  s_err[128] = "";

// Quiz list.
static QuizEntry s_quizzes[MAX_QUIZZES];
static int       s_nQuizzes = 0;
static int       s_sel      = 0;
static int       s_scroll   = 0;
static char      s_selName[96] = "";

// Current quiz.
static char  s_quizBuf[QUIZ_BUF];
static JsDoc s_quizDoc;
static int   s_qTotal   = 0;
static int   s_qIndex   = 0;
static char  s_quizName[96] = "";
static int   s_score    = 0;
static int   s_possible = 0;

// Current question.
static int      s_answersN = 0;
static char     s_answers[MAX_ANSWERS][ANS_LEN];
static bool     s_correctIdx[MAX_ANSWERS];
static char     s_qtype[12]  = "mc";
static double   s_sliderMin  = 0.0;
static double   s_sliderMax  = 100.0;
static double   s_sliderTol  = 0.0;
static double   s_sliderAns  = 0.0;
static double   s_sliderVal  = 50.0;
static char     s_typeAns[128] = "";
static int      s_selAns    = -1;
static int      s_puzzleOrder[MAX_ANSWERS];
static int      s_puzzleN   = 0;
static unsigned s_qStartMs  = 0;
static bool     s_answered  = false;
static int      s_gotPoints = 0;
static bool     s_wasCorrect = false;
static char     s_revealAnswer[ANS_LEN] = "";

// Question image.
static char s_imgBuf[IMG_BUF];
static u16  s_imgW = 0, s_imgH = 0;
static bool s_imgGif = false;

// ------------------------------------------------------------------ helpers

static void fail(const char* msg)
{
	snprintf(s_err, sizeof(s_err), "%s", msg);
	s_phase = PH_ERROR;
}

// Modal text entry using the touch keyboard. Returns true on CONFIRM.
static bool ask_text(const char* hint, char* out, int outsz)
{
	bool done = false;
	bool ok = false;
	while (1) {
		scanKeys();
		if (keysDown() & KEY_B) break;
		touchPosition t;
		touchRead(&t);
		if (keysDown() & KEY_TOUCH) {
			if (kbd_touch(&t, out, outsz, &done)) {
				ok = done;
				break;
			}
		}
		kbd_draw(out);
		net_wait_vblank();
	}
	return ok;
}

static bool progress_top(void* user, int received, int total)
{
	(void)user;
	gfx_set_target_top();
	gfx_clear(COL_BG);
	gfx_text_center(DS_TOP_W / 2, 60, COL_TEXT, "Talking to the server...", 1);
	gfx_rect(28, 84, DS_TOP_W - 56, 12, COL_BAR);
	if (total > 0) {
		float pct = (float)received / (float)total;
		if (pct > 1.0f) pct = 1.0f;
		gfx_rect(28, 84, (int)((DS_TOP_W - 56) * pct), 12, COL_GOLD);
	}
	scanKeys();
	if (keysDown() & KEY_B) return false; // cancel
	net_wait_vblank();
	return true;
}

// ------------------------------------------------------------------ loading

static void fetch_list(void)
{
	static char buf[16384]; // static: too big for the 16KB DS stack
	int n = net_http_get(s_host, s_port, "/api/quizzes", buf, sizeof(buf),
	                     8000, progress_top, NULL);
	if (n < 0) {
		fail(n == -2 ? "Cancelled" : "Could not load the quiz list");
		return;
	}
	static JsDoc doc; // large; keep it static
	if (!json_parse(&doc, buf)) {
		fail("The server sent a bad reply");
		return;
	}
	int arr = json_obj_get(&doc, 0, "quizzes");
	int cnt = json_arr_len(&doc, arr);
	s_nQuizzes = 0;
	for (int i = 0; i < cnt && s_nQuizzes < MAX_QUIZZES; i++) {
		int q = json_arr_at(&doc, arr, i);
		const char* name = json_ostr(&doc, q, "name", "");
		if (!name[0]) continue;
		snprintf(s_quizzes[s_nQuizzes].name, sizeof(s_quizzes[s_nQuizzes].name),
		         "%s", name);
		s_quizzes[s_nQuizzes].banner = json_obool(&doc, q, "banner", false);
		s_nQuizzes++;
	}
	if (s_nQuizzes == 0) {
		fail("No quizzes found");
		return;
	}
	s_sel = 0;
	s_scroll = 0;
	snprintf(s_selName, sizeof(s_selName), "%s", s_quizzes[0].name);
	s_phase = PH_LIST;
}

// ------------------------------------------------------------------ question

static int current_question_node(void)
{
	return json_arr_at(&s_quizDoc, 0, s_qIndex);
}

static bool is_gif_url(const char* url)
{
	const char* dot = strrchr(url, '.');
	return dot && !strcasecmp(dot, ".gif");
}

// Swaps the URL's extension for .ds and fetches it into s_imgBuf.
static void fetch_image(const char* url)
{
	s_imgW = s_imgH = 0;
	s_imgGif = false;
	if (!url || !url[0]) return;

	if (is_gif_url(url)) {
		s_imgGif = true; // show GIF_MSG, no download
		return;
	}

	char path[256];
	snprintf(path, sizeof(path), "%s", url);
	char* dot = strrchr(path, '.');
	if (dot) snprintf(dot, sizeof(path) - (size_t)(dot - path), "%s", ".ds");

	int n = net_http_get(s_host, s_port, path, s_imgBuf, sizeof(s_imgBuf),
	                     20000, progress_top, NULL);
	if (n >= 4) {
		u16 w = (u16)((u8)s_imgBuf[0] | ((u8)s_imgBuf[1] << 8));
		u16 h = (u16)((u8)s_imgBuf[2] | ((u8)s_imgBuf[3] << 8));
		if (w > 0 && h > 0 && 4 + (int)w * h * 2 <= n) {
			s_imgW = w;
			s_imgH = h;
		}
	}
}

static void load_question(int qnode)
{
	const char* type  = json_ostr(&s_quizDoc, qnode, "type", "type");

	snprintf(s_qtype, sizeof(s_qtype), "%s", type);
	s_answersN = 0;
	s_selAns = -1;
	s_answered = false;
	s_typeAns[0] = '\0';
	s_puzzleN = 0;
	s_imgW = s_imgH = 0;
	s_imgGif = false;
	for (int i = 0; i < MAX_ANSWERS; i++) {
		s_answers[i][0] = '\0';
		s_correctIdx[i] = false;
		s_puzzleOrder[i] = -1;
	}

	int ansArr = json_obj_get(&s_quizDoc, qnode, "answers");
	int cnt = json_arr_len(&s_quizDoc, ansArr);
	if (cnt > MAX_ANSWERS) cnt = MAX_ANSWERS;
	for (int i = 0; i < cnt; i++) {
		const char* a = json_str(&s_quizDoc, json_arr_at(&s_quizDoc, ansArr, i), "");
		snprintf(s_answers[i], sizeof(s_answers[i]), "%s", a);
	}
	s_answersN = cnt;

	// Correct answers per type.
	if (!strcmp(s_qtype, "mc")) {
		int ci = json_obj_get(&s_quizDoc, qnode, "correct_indices");
		int cn = json_arr_len(&s_quizDoc, ci);
		for (int i = 0; i < cn && i < MAX_ANSWERS; i++) {
			int idx = (int)json_num(&s_quizDoc, json_arr_at(&s_quizDoc, ci, i), -1);
			if (idx >= 0 && idx < MAX_ANSWERS) s_correctIdx[idx] = true;
		}
	} else if (!strcmp(s_qtype, "slider")) {
		s_sliderMin = json_onum(&s_quizDoc, qnode, "min", 0);
		s_sliderMax = json_onum(&s_quizDoc, qnode, "max", 100);
		s_sliderTol = json_onum(&s_quizDoc, qnode, "tolerance", 0);
		s_sliderAns = json_onum(&s_quizDoc, qnode, "answer", 0);
		if (s_sliderMax <= s_sliderMin) s_sliderMax = s_sliderMin + 1.0;
		s_sliderVal = (s_sliderMin + s_sliderMax) / 2.0;
	}

	// Reveal text for the answer line.
	if (!strcmp(s_qtype, "mc")) {
		s_revealAnswer[0] = '\0';
		for (int i = 0; i < s_answersN; i++) {
			if (s_correctIdx[i]) {
				snprintf(s_revealAnswer, sizeof(s_revealAnswer), "%s", s_answers[i]);
				break;
			}
		}
	} else if (!strcmp(s_qtype, "slider")) {
		snprintf(s_revealAnswer, sizeof(s_revealAnswer), "%g", s_sliderAns);
	} else {
		snprintf(s_revealAnswer, sizeof(s_revealAnswer), "%s",
		         json_ostr(&s_quizDoc, qnode, "answer", ""));
	}

	// Question image (low-res .ds).
	int imgs = json_obj_get(&s_quizDoc, qnode, "images");
	int img0 = json_arr_at(&s_quizDoc, imgs, 0);
	const char* url = NULL;
	if (img0 >= 0) {
		int urlNode = json_obj_get(&s_quizDoc, img0, "url");
		url = json_str(&s_quizDoc, urlNode, NULL);
	}
	fetch_image(url);

	s_qStartMs = net_ticks_ms();
}

static int points_on_offer(void)
{
	int sec = (int)((net_ticks_ms() - s_qStartMs) / 1000u);
	int pts = 1000 - sec * 50;
	return pts < 0 ? 0 : pts;
}

static bool check_answer(void)
{
	if (!strcmp(s_qtype, "mc")) {
		return s_selAns >= 0 && s_selAns < s_answersN && s_correctIdx[s_selAns];
	}
	if (!strcmp(s_qtype, "slider")) {
		return fabs(s_sliderVal - s_sliderAns) <= s_sliderTol + 1e-9;
	}
	if (!strcmp(s_qtype, "puzzle")) {
		if (s_puzzleN != s_answersN) return false;
		for (int i = 0; i < s_puzzleN; i++) {
			if (s_puzzleOrder[i] != i) return false;
		}
		return true;
	}
	// type-in: case-insensitive
	return strcasecmp(s_typeAns,
	                  json_ostr(&s_quizDoc, current_question_node(), "answer", "")) == 0;
}

static void submit(void)
{
	if (s_answered) return;
	if (!strcmp(s_qtype, "mc") && s_selAns < 0) return;
	if (!strcmp(s_qtype, "type") && !s_typeAns[0]) return;
	if (!strcmp(s_qtype, "puzzle") && s_puzzleN != s_answersN) return;

	s_wasCorrect = check_answer();
	s_gotPoints = s_wasCorrect ? points_on_offer() : 0;
	s_score += s_gotPoints;
	s_possible += 1000;
	s_answered = true;
	s_phase = PH_REVEAL;
}

static void next_question(void)
{
	if (s_qIndex + 1 >= s_qTotal) {
		s_phase = PH_RESULTS;
		return;
	}
	s_qIndex++;
	load_question(current_question_node());
	s_phase = PH_QUESTION;
}

static void start_quiz(void)
{
	char path[512];
	char enc[96];
	net_urlencode(s_quizzes[s_sel].name, enc, sizeof(enc));
	snprintf(path, sizeof(path), "/api/quiz/%s", enc);

	int n = net_http_get(s_host, s_port, path, s_quizBuf, sizeof(s_quizBuf),
	                     8000, progress_top, NULL);
	if (n < 0) {
		fail(n == -2 ? "Cancelled" : "Could not load that quiz");
		return;
	}
	if (!json_parse(&s_quizDoc, s_quizBuf)) {
		fail("The server sent a bad reply");
		return;
	}
	s_qTotal = json_arr_len(&s_quizDoc, 0);
	if (s_qTotal <= 0) {
		fail("Could not load that quiz: it has no questions");
		return;
	}
	snprintf(s_quizName, sizeof(s_quizName), "%s", s_quizzes[s_sel].name);
	s_qIndex = 0;
	s_score = 0;
	s_possible = 0;
	load_question(current_question_node());
	s_phase = PH_QUESTION;
}

// ------------------------------------------------------------------ step

void solo_enter(const char* host, int port)
{
	snprintf(s_host, sizeof(s_host), "%s", host);
	s_port = port;
	s_phase = PH_LOAD_LIST;
	s_err[0] = '\0';
	fetch_list();
}

void solo_step(void)
{
	if (s_phase == PH_LOAD_QUIZ) {
		start_quiz();
		return;
	}

	scanKeys();
	u32 down = keysDown();
	touchPosition touch;
	touchRead(&touch);
	bool tapped = (down & KEY_TOUCH) != 0;

	switch (s_phase) {
	case PH_LIST: {
		if (down & KEY_B) { s_phase = PH_ERROR; s_err[0] = '\0'; return; }
		if (down & KEY_UP)   { if (s_sel > 0) s_sel--; }
		if (down & KEY_DOWN) { if (s_sel < s_nQuizzes - 1) s_sel++; }
		if (s_sel < s_scroll) s_scroll = s_sel;
		if (s_sel >= s_scroll + 8) s_scroll = s_sel - 7;
		bool play = down & KEY_A;
		if (tapped) {
			for (int v = 0; v < 8; v++) {
				int pos = s_scroll + v;
				if (pos >= s_nQuizzes) break;
				int y = 24 + v * 21;
				if (touch.px >= 8 && touch.px < 248 &&
				    touch.py >= y && touch.py < y + 21) {
					s_sel = pos;
					play = true;
					break;
				}
			}
		}
		if (s_sel >= 0 && s_sel < s_nQuizzes) {
			snprintf(s_selName, sizeof(s_selName), "%s", s_quizzes[s_sel].name);
		}
		if (play) {
			s_phase = PH_LOAD_QUIZ;
		}
		break;
	}

	case PH_QUESTION: {
		if (down & KEY_B) { s_phase = PH_LIST; return; }

		if (!strcmp(s_qtype, "mc")) {
			if (down & KEY_UP)   { if (s_selAns >= 2) s_selAns -= 2; else if (s_selAns < 0) s_selAns = 0; }
			if (down & KEY_DOWN) { if (s_selAns < 0) s_selAns = 0; else if (s_selAns + 2 < s_answersN) s_selAns += 2; }
			if (down & KEY_LEFT) { if (s_selAns > 0) s_selAns--; }
			if (down & KEY_RIGHT){ if (s_selAns + 1 < s_answersN) s_selAns++; }
			if (tapped) {
				// 2x2 tile grid
				for (int i = 0; i < s_answersN && i < 4; i++) {
					int r = i / 2, c = i % 2;
					int x = 8 + c * 124, y = 26 + r * 54;
					if (touch.px >= x && touch.px < x + 116 &&
					    touch.py >= y && touch.py < y + 50) {
						s_selAns = i;
					}
				}
				if (touch.px >= 8 && touch.px < 248 &&
				    touch.py >= 140 && touch.py < 176) submit();
			}
			if (down & KEY_A) submit();
		} else if (!strcmp(s_qtype, "slider")) {
			if (down & KEY_LEFT)  s_sliderVal -= (s_sliderMax - s_sliderMin) / 20.0;
			if (down & KEY_RIGHT) s_sliderVal += (s_sliderMax - s_sliderMin) / 20.0;
			if (s_sliderVal < s_sliderMin) s_sliderVal = s_sliderMin;
			if (s_sliderVal > s_sliderMax) s_sliderVal = s_sliderMax;
			if (tapped) {
				if (touch.px >= 8 && touch.px < 44 && touch.py >= 128 && touch.py < 158)
					s_sliderVal -= (s_sliderMax - s_sliderMin) / 20.0;
				if (touch.px >= 212 && touch.px < 248 && touch.py >= 128 && touch.py < 158)
					s_sliderVal += (s_sliderMax - s_sliderMin) / 20.0;
				if (s_sliderVal < s_sliderMin) s_sliderVal = s_sliderMin;
				if (s_sliderVal > s_sliderMax) s_sliderVal = s_sliderMax;
				if (touch.px >= 8 && touch.px < 248 &&
				    touch.py >= 164 && touch.py < 188) submit();
			}
			if (down & KEY_A) submit();
		} else if (!strcmp(s_qtype, "puzzle")) {
			if (tapped) {
				for (int i = 0; i < s_answersN; i++) {
					int y = 26 + i * 20;
					if (touch.px >= 8 && touch.px < 248 &&
					    touch.py >= y && touch.py < y + 18) {
						// Toggle the answer's position in the order.
						bool found = false;
						for (int k = 0; k < s_puzzleN; k++) {
							if (s_puzzleOrder[k] == i) {
								// remove it
								for (int m = k; m < s_puzzleN - 1; m++)
									s_puzzleOrder[m] = s_puzzleOrder[m + 1];
								s_puzzleN--;
								found = true;
								break;
							}
						}
						if (!found && s_puzzleN < MAX_ANSWERS) {
							s_puzzleOrder[s_puzzleN++] = i;
						}
					}
				}
				if (touch.px >= 8 && touch.px < 248 &&
				    touch.py >= 164 && touch.py < 188) submit();
			}
			if (down & KEY_A) submit();
		} else {
			// type-in
			if (tapped && touch.px >= 8 && touch.px < 248 &&
			    touch.py >= 60 && touch.py < 100) {
				ask_text("Type your answer", s_typeAns, sizeof(s_typeAns));
			}
			if (tapped && touch.px >= 8 && touch.px < 248 &&
			    touch.py >= 140 && touch.py < 176) submit();
			if (down & KEY_A) {
				ask_text("Type your answer", s_typeAns, sizeof(s_typeAns));
			}
		}
		break;
	}

	case PH_REVEAL: {
		if (down & KEY_A || tapped) next_question();
		if (down & KEY_B) { s_phase = PH_LIST; }
		break;
	}

	case PH_RESULTS: {
		if (down & KEY_A || tapped) {
			s_qIndex = 0;
			s_phase = PH_LIST;
		}
		if (down & KEY_B) { s_phase = PH_ERROR; s_err[0] = '\0'; }
		break;
	}

	case PH_ERROR: {
		if (down & KEY_A || down & KEY_B || tapped) {
			s_phase = PH_ERROR; // caller (main) sees wants_exit and leaves
		}
		break;
	}

	default:
		break;
	}
}

bool solo_wants_exit(void)
{
	// The error phase doubles as "exit to setup" once dismissed.
	return s_phase == PH_ERROR && s_err[0] == '\0';
}

void solo_exit(void)
{
	s_phase = PH_LOAD_LIST;
	s_err[0] = '\0';
}

// ------------------------------------------------------------------ draw

static void draw_top_question(void)
{
	int qnode = current_question_node();
	const char* qtext = json_ostr(&s_quizDoc, qnode, "question", "");

	gfx_clear(COL_BG);

	char head[64];
	snprintf(head, sizeof(head), "Q%d/%d  %s", s_qIndex + 1, s_qTotal, s_quizName);
	gfx_text_center(DS_TOP_W / 2, 4, COL_DIM, head, 1);

	char score[48];
	snprintf(score, sizeof(score), "%d pts", s_score);
	gfx_text(DS_TOP_W - 8 - gfx_text_width(score, 1), 4, COL_TEXT, score);

	// Question text, wrapped and auto-scaled.
	char lines[6][QLINE_LEN];
	int scale = 2;
	int n = gfx_wrap_fit(qtext, DS_TOP_W - 24, 64, 2, 1, lines, 6, &scale);
	int ty = s_imgW > 0 ? 18 : 40;
	for (int i = 0; i < n; i++) {
		gfx_text_center(DS_TOP_W / 2, ty + i * (8 * scale + 4), COL_TEXT, lines[i], scale);
	}

	// Image or GIF notice.
	if (s_imgW > 0) {
		gfx_blit555((DS_TOP_W - s_imgW) / 2, 116 - s_imgH / 2, s_imgW, s_imgH,
		            (const u16*)(s_imgBuf + 4));
	} else if (s_imgGif) {
		int gscale = 1;
		char glines[4][QLINE_LEN];
		int gn = gfx_wrap_fit(GIF_MSG, DS_TOP_W - 24, 40, 1, 1, glines, 4, &gscale);
		gfx_rect(8, 100, DS_TOP_W - 16, 48, COL_PANEL);
		for (int i = 0; i < gn; i++) {
			gfx_text_center(DS_TOP_W / 2, 108 + i * 10, COL_DIM, glines[i], 1);
		}
	}

	// Points window.
	int pts = points_on_offer();
	char pbuf[40];
	snprintf(pbuf, sizeof(pbuf), "%d points available", pts);
	gfx_text_center(DS_TOP_W / 2, 166, COL_DIM, pbuf, 1);
	gfx_bar(28, 178, DS_TOP_W - 56, 6, pts / 1000.0f, COL_GOLD, COL_BAR);
}

static void draw_bottom_mc(void)
{
	for (int i = 0; i < s_answersN && i < 4; i++) {
		int r = i / 2, c = i % 2;
		int x = 8 + c * 124, y = 26 + r * 54;
		u8 fill;
		switch (i & 3) {
		case 0: fill = COL_GOLD; break;
		case 1: fill = COL_PINK; break;
		case 2: fill = COL_GOOD; break;
		default: fill = COL_BAD; break;
		}
		if (s_selAns == i) gfx_rect(x - 1, y - 1, 118, 52, COL_TEXT);
		gfx_rect(x, y, 116, 50, fill);
		char lines[3][QLINE_LEN];
		int sc = 1;
		int n = gfx_wrap_fit(s_answers[i], 108, 42, 1, 1, lines, 3, &sc);
		for (int k = 0; k < n; k++) {
			gfx_text_center(x + 58, y + 4 + k * 10, COL_INK, lines[k], 1);
		}
	}
	gfx_rect(8, 140, DS_BOT_W - 16, 36, COL_GOOD);
	gfx_text_center(DS_BOT_W / 2, 152, COL_INK, "SUBMIT", 1);
	gfx_text_center(DS_BOT_W / 2, 182, COL_DIM, "B = back to the list", 1);
}

static void draw_bottom_slider(void)
{
	char val[32];
	snprintf(val, sizeof(val), "%.1f", s_sliderVal);
	gfx_text_center(DS_BOT_W / 2, 30, COL_TEXT, "DRAG THE SLIDER", 1);
	gfx_text_center(DS_BOT_W / 2, 60, COL_GOLD, val, 2);

	float pct = (float)((s_sliderVal - s_sliderMin) / (s_sliderMax - s_sliderMin));
	gfx_bar(50, 100, 156, 8, pct, COL_GOLD, COL_BAR);

	gfx_rect(8, 128, 36, 30, COL_PANEL);
	gfx_text_center(26, 138, COL_TEXT, "-", 1);
	gfx_rect(212, 128, 36, 30, COL_PANEL);
	gfx_text_center(230, 138, COL_TEXT, "+", 1);

	gfx_rect(8, 164, DS_BOT_W - 16, 24, COL_GOOD);
	gfx_text_center(DS_BOT_W / 2, 172, COL_INK, "SUBMIT", 1);
	gfx_text_center(DS_BOT_W / 2, 190, COL_DIM, "B = back to the list", 1);
}

static void draw_bottom_puzzle(void)
{
	gfx_text_center(DS_BOT_W / 2, 4, COL_DIM, "PUT THEM IN ORDER - tap in order", 1);
	for (int i = 0; i < s_answersN; i++) {
		int y = 18 + i * 17;
		// numbered badge if placed
		char label[QLINE_LEN];
		bool placed = false;
		int pos = 0;
		for (int k = 0; k < s_puzzleN; k++) {
			if (s_puzzleOrder[k] == i) { placed = true; pos = k + 1; break; }
		}
		u8 fill = placed ? COL_GOLD : COL_PANEL;
		gfx_rect(8, y, DS_BOT_W - 16, 15, fill);
		if (placed) {
			snprintf(label, sizeof(label), "%d. %s", pos, s_answers[i]);
		} else {
			snprintf(label, sizeof(label), "%s", s_answers[i]);
		}
		gfx_text(12, y + 4, placed ? COL_INK : COL_TEXT, label);
	}
	gfx_rect(8, 164, DS_BOT_W - 16, 24, COL_GOOD);
	gfx_text_center(DS_BOT_W / 2, 172, COL_INK, "SUBMIT", 1);
	gfx_text_center(DS_BOT_W / 2, 190, COL_DIM, "B = back to the list", 1);
}

static void draw_bottom_type(void)
{
	gfx_text_center(DS_BOT_W / 2, 40, COL_DIM, "TYPE AN ANSWER", 1);
	gfx_rect(8, 60, DS_BOT_W - 16, 40, COL_PANEL);
	if (s_typeAns[0]) {
		gfx_text_center(DS_BOT_W / 2, 76, COL_TEXT, s_typeAns, 1);
	} else {
		gfx_text_center(DS_BOT_W / 2, 76, COL_DIM, "tap to type", 1);
	}
	gfx_rect(8, 140, DS_BOT_W - 16, 36, COL_GOOD);
	gfx_text_center(DS_BOT_W / 2, 152, COL_INK, "SUBMIT", 1);
	gfx_text_center(DS_BOT_W / 2, 182, COL_DIM, "B = back to the list", 1);
}

void solo_draw_top(void)
{
	gfx_set_target_top();
	switch (s_phase) {
	case PH_LOAD_LIST:
	case PH_LOAD_QUIZ:
		// progress_top draws during the fetch; draw a stable frame here
		gfx_clear(COL_BG);
		gfx_text_center(DS_TOP_W / 2, 60, COL_TEXT, "Talking to the server...", 1);
		break;
	case PH_LIST:
		gfx_clear(COL_BG);
		gfx_text_center(DS_TOP_W / 2, 30, COL_TEXT, "SOLO QUIZ", 2);
		{
			char cnt[48];
			snprintf(cnt, sizeof(cnt), "%d quiz%s on the server",
			         s_nQuizzes, s_nQuizzes == 1 ? "" : "zes");
			gfx_text_center(DS_TOP_W / 2, 60, COL_DIM, cnt, 1);
		}
		{
			char sel[120];
			snprintf(sel, sizeof(sel), "Selected: %s", s_selName);
			gfx_text_center(DS_TOP_W / 2, 84, COL_GOLD, sel, 1);
		}
		gfx_text_center(DS_TOP_W / 2, 110, COL_DIM, "Tap a quiz on the touch screen", 1);
		gfx_text_center(DS_TOP_W / 2, 140, COL_DIM, "A = play", 1);
		gfx_text_center(DS_TOP_W / 2, 176, COL_DIM, "B = back to setup", 1);
		break;
	case PH_QUESTION:
		draw_top_question();
		break;
	case PH_REVEAL:
		gfx_clear(COL_BG);
		gfx_text_center(DS_TOP_W / 2, 30, s_wasCorrect ? COL_GOOD : COL_BAD,
		                s_wasCorrect ? "CORRECT!" : "NOT QUITE", 2);
		{
			char pts[48];
			snprintf(pts, sizeof(pts), "+%d points", s_gotPoints);
			gfx_text_center(DS_TOP_W / 2, 70, COL_GOLD, pts, 1);
		}
		{
			char ans[160];
			snprintf(ans, sizeof(ans), "Answer: %s", s_revealAnswer);
			gfx_text_center(DS_TOP_W / 2, 100, COL_DIM, ans, 1);
		}
		gfx_text_center(DS_TOP_W / 2, 150, COL_DIM, "A = next question", 1);
		gfx_text_center(DS_TOP_W / 2, 176, COL_DIM, "B = back to the list", 1);
		break;
	case PH_RESULTS:
		gfx_clear(COL_BG);
		gfx_text_center(DS_TOP_W / 2, 24, COL_TEXT, "QUIZ COMPLETE", 2);
		{
			char sc[48];
			snprintf(sc, sizeof(sc), "You scored %d points", s_score);
			gfx_text_center(DS_TOP_W / 2, 80, COL_TEXT, sc, 1);
		}
		{
			char po[48];
			snprintf(po, sizeof(po), "out of a possible %d", s_possible);
			gfx_text_center(DS_TOP_W / 2, 100, COL_DIM, po, 1);
		}
		gfx_text_center(DS_TOP_W / 2, 150, COL_DIM, "A = back to the list", 1);
		gfx_text_center(DS_TOP_W / 2, 176, COL_DIM, "B = back to setup", 1);
		break;
	case PH_ERROR:
		gfx_clear(COL_BG);
		gfx_text_center(DS_TOP_W / 2, 60, COL_BAD, "COULD NOT LOAD", 2);
		if (s_err[0]) gfx_text_center(DS_TOP_W / 2, 100, COL_DIM, s_err, 1);
		gfx_text_center(DS_TOP_W / 2, 150, COL_DIM, "Press A or B to go back", 1);
		break;
	}
}

void solo_draw_bottom(void)
{
	gfx_set_target_bottom();
	switch (s_phase) {
	case PH_LOAD_LIST:
		gfx_clear(COL_BG);
		gfx_text_center(DS_BOT_W / 2, 90, COL_DIM, "Fetching the quiz list...", 1);
		break;
	case PH_LOAD_QUIZ:
		gfx_clear(COL_BG);
		gfx_text_center(DS_BOT_W / 2, 90, COL_DIM, "Fetching questions...", 1);
		break;
	case PH_LIST: {
		gfx_clear(COL_BG);
		gfx_text(8, 4, COL_TEXT, "QUIZZES");
		for (int v = 0; v < 8; v++) {
			int pos = s_scroll + v;
			if (pos >= s_nQuizzes) break;
			int y = 24 + v * 21;
			bool sel = (pos == s_sel);
			gfx_rect(8, y, DS_BOT_W - 16, 21, sel ? COL_GOLD : COL_PANEL);
			gfx_text(12, y + 6, sel ? COL_INK : COL_TEXT, s_quizzes[pos].name);
		}
		break;
	}
	case PH_QUESTION: {
		gfx_clear(COL_BG);
		if (!strcmp(s_qtype, "mc")) draw_bottom_mc();
		else if (!strcmp(s_qtype, "slider")) draw_bottom_slider();
		else if (!strcmp(s_qtype, "puzzle")) draw_bottom_puzzle();
		else draw_bottom_type();
		break;
	}
	case PH_REVEAL: {
		gfx_clear(COL_BG);
		gfx_rect(8, 70, DS_BOT_W - 16, 52, s_wasCorrect ? COL_GOOD : COL_BAD);
		gfx_text_center(DS_BOT_W / 2, 88, COL_INK,
		                s_wasCorrect ? "CORRECT!" : "NOT QUITE", 1);
		char pts[40];
		snprintf(pts, sizeof(pts), "+%d points", s_gotPoints);
		gfx_text_center(DS_BOT_W / 2, 102, COL_INK, pts, 1);
		gfx_rect(8, 140, DS_BOT_W - 16, 36, COL_GOOD);
		gfx_text_center(DS_BOT_W / 2, 152, COL_INK, "NEXT", 1);
		gfx_text_center(DS_BOT_W / 2, 182, COL_DIM, "B = back to the list", 1);
		break;
	}
	case PH_RESULTS: {
		gfx_clear(COL_BG);
		gfx_rect(8, 70, DS_BOT_W - 16, 44, COL_GOOD);
		gfx_text_center(DS_BOT_W / 2, 84, COL_INK, "BACK TO THE LIST", 1);
		gfx_text_center(DS_BOT_W / 2, 140, COL_DIM, "B = back to setup", 1);
		break;
	}
	case PH_ERROR:
		gfx_clear(COL_BG);
		gfx_rect(8, 70, DS_BOT_W - 16, 44, COL_BAD);
		gfx_text_center(DS_BOT_W / 2, 84, COL_INK, "BACK", 1);
		break;
	}
}
