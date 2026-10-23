// QuizFabric 3DS — a join-a-live-quiz client for the QuizFabric Flask server.
//
// Talks Socket.IO v5 / Engine.IO v4 over a raw WebSocket. Top screen shows the
// question, bottom screen holds the answer tiles / text box and SUBMIT.

#include <3ds.h>
#include <citro2d.h>

#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cachedel.h"
#include "control.h"
#include "delete.h"
#include "host.h"
#include "json.h"
#include "sio.h"
#include "solo.h"
#include "steal.h"
#include "thumb.h"
#include "ui.h"
#include "ws.h"

#define CFG_DIR  "sdmc:/3ds/quizfabric"
#define CFG_PATH "sdmc:/3ds/quizfabric/config.txt"

#define MAX_ANSWERS 8
#define ANS_LEN     128
#define QTEXT_LEN   640
#define MAX_BOARD   32

// The server awards max(0, 1000 - seconds*50), so points hit zero at 20s.
#define POINT_WINDOW_SEC 20.0f

// Bump this on every change so the setup screen can prove which CIA is on
// the console. (7 = tiled-RGBA8 image fix, 45 = close_rst recursion fix)
#define APP_BUILD "50"

// The production build talks to one fixed server; the dev build keeps the
// editable server field.
#ifdef PROD_BUILD
#define PROD_HOST "play.lucasgeitgey.com"
#define PROD_PORT 80
#endif

// The 3DS SOC stack often refuses early connection attempts; retry back to
// back indefinitely. B cancels, so there is no failure path to worry about.
#define RETRY_DELAY_MS 0

typedef enum { QT_MC = 0, QT_TYPE, QT_SLIDER, QT_PUZZLE } QType;

typedef enum {
	ST_MENU = 0,
	ST_CONNECTING,
	ST_LOBBY,
	ST_QUESTION,
	ST_WAITING,
	ST_REVEALED,
	ST_LEADERBOARD,
	ST_FINAL,
	ST_CONTROL,   // host control panel (see control.c)
	ST_SOLO,      // solo quiz mode (see solo.c)
	ST_HOST,      // hosting from the 3DS (see host.c)
	ST_JOIN_SETUP,// join sub-page: name / PIN / avatar
	ST_STEAL,     // steal a Kahoot quiz (see steal.c)
	ST_DELETE,    // delete own quizzes (see delete.c)
	ST_CACHEDEL,  // offline: delete cached quizzes (see cachedel.c)
	ST_ERROR
} AppState;

typedef struct {
	char name[48];
	int  score;
	int  lastCorrect; // -1 unknown, 0 wrong, 1 right
	bool left;
} BoardRow;

typedef struct {
	// Configuration
	char host[80];
	int  port;
	char player[48];
	char code[8];
	char avatar[64];

	// Connection
	WsConn*  ws;
	bool     nsSent;      // namespace CONNECT emitted
	bool     joinSent;    // join_game emitted
	AppState state;
	bool     offline;     // offline mode: cached quizzes only
	bool     bootPing;    // startup connectivity check still running
	bool     pingToOnline;// Y-check from offline mode is in flight
	char     error[160];
	char     quizName[96];

	// Current question
	char  question[QTEXT_LEN];
	bool  isMC;
	char  answers[MAX_ANSWERS][ANS_LEN];
	int   nAnswers;
	bool  hasImages;
	int   qNumber;
	u64   qStartMs;

	// Slider / puzzle extras
	int   qType;     // QT_MC / QT_TYPE / QT_SLIDER / QT_PUZZLE
	float sMin, sMax, sValue;          // slider state
	int   pOrder[MAX_ANSWERS];         // puzzle: indices into answers, player order
	int   pSel;                        // puzzle: selected row

	// Local answer
	int   selected;          // MC index, -1 if none
	char  typed[ANS_LEN];    // type-in text
	char  submittedText[ANS_LEN];

	// Reveal
	bool  revealCorrect;
	char  correctAnswer[ANS_LEN];
	int   myScore;
	int   lastGain;
	u64   revealUntil; // when to slide from the personal card to the standings

	// Leaderboard
	BoardRow board[MAX_BOARD];
	int      nBoard;
	int      boardScroll;

	// Transient toast
	char  toast[128];
	u64   toastUntil;

	// Error screen: retry offered only for game-connection errors
	bool  errorRetry;

	// Connect auto-retry (the 3DS SOC stack often refuses the first tries)
	int   retries;
	u64   retryAt;

	float time; // seconds since boot, for animation
} App;

static App     G;
static SioMsg  g_msg;   // large (embeds a JsDoc); must not live on the stack
static char    g_rx[32768];

// ------------------------------------------------------------------ helpers

static void toast(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(G.toast, sizeof(G.toast), fmt, ap);
	va_end(ap);
	G.toastUntil = osGetTime() + 3000;
}

static void set_error(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(G.error, sizeof(G.error), fmt, ap);
	va_end(ap);
	G.errorRetry = true; // game-connection errors can be retried
	G.state = ST_ERROR;
}

// Shared error screen for every mode (see app.h). Unlike set_error() there is
// no retry path - the user is sent back to the menu.
void app_error(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(G.error, sizeof(G.error), fmt, ap);
	va_end(ap);
	G.errorRetry = false;
	G.state = ST_ERROR;
}

static void net_teardown(void)
{
	if (G.ws) {
		ws_close(G.ws);
		G.ws = NULL;
	}
	G.nsSent   = false;
	G.retries  = 0;
	G.retryAt  = 0;
	G.joinSent = false;
}

static void config_load(void)
{
	snprintf(G.host, sizeof(G.host), "%s", "192.168.1.100");
	G.port = 5710;
	snprintf(G.player, sizeof(G.player), "%s", "3DS Player");
	snprintf(G.code, sizeof(G.code), "%s", "");
	snprintf(G.avatar, sizeof(G.avatar), "%s", "Pusheen");

	FILE* f = fopen(CFG_PATH, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		const char* k = line;
		const char* v = eq + 1;
		if      (!strcmp(k, "host"))   snprintf(G.host, sizeof(G.host), "%s", v);
		else if (!strcmp(k, "port"))   G.port = atoi(v);
		else if (!strcmp(k, "name"))   snprintf(G.player, sizeof(G.player), "%s", v);
		else if (!strcmp(k, "avatar")) snprintf(G.avatar, sizeof(G.avatar), "%s", v);
		else if (!strcmp(k, "code"))   snprintf(G.code, sizeof(G.code), "%s", v);
	}
	fclose(f);
	if (G.port <= 0 || G.port > 65535) G.port = 5710;
}

static void config_save(void)
{
	mkdir("sdmc:/3ds", 0777);
	mkdir(CFG_DIR, 0777);
	FILE* f = fopen(CFG_PATH, "w");
	if (!f) return;
	fprintf(f, "host=%s\nport=%d\nname=%s\navatar=%s\ncode=%s\n",
	        G.host, G.port, G.player, G.avatar, G.code);
	fclose(f);
}

static bool keyboard(const char* hint, const char* initial,
                     char* out, int outsz, SwkbdType type, int maxLen)
{
	SwkbdState sw;
	swkbdInit(&sw, type, 2, maxLen);
	swkbdSetHintText(&sw, hint);
	if (initial && *initial) swkbdSetInitialText(&sw, initial);
	swkbdSetValidation(&sw, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);

	char tmp[256];
	SwkbdButton b = swkbdInputText(&sw, tmp, sizeof(tmp));
	if (b != SWKBD_BUTTON_CONFIRM) return false;
	snprintf(out, outsz, "%s", tmp);
	return true;
}

// --------------------------------------------------------------- avatar list

#define MAX_AVATARS 40
static char s_avatars[MAX_AVATARS][64];
static int  s_nAvatars = 0;

static void avatars_defaults(void)
{
	static const char* fallback[] = {
		"Pusheen", "Pig", "Steve", "Alex", "Cat Emoji",
		"Doggy Emoji", "Chicken Emoji", "Baby Seal", "Donkey", "Pusheenicorn"
	};
	s_nAvatars = (int)(sizeof(fallback) / sizeof(fallback[0]));
	for (int i = 0; i < s_nAvatars; i++) {
		snprintf(s_avatars[i], sizeof(s_avatars[i]), "%s", fallback[i]);
	}
}

// Pulls the real avatar list from the server so names match what the host
// screen renders. Runs in the background (frame-driven) so the menu never
// freezes on the slow Wi-Fi; falls back to a built-in list when offline.
static HttpGet* g_avGet = NULL;

static void avatars_fetch(void)
{
	if (g_avGet) return;
	g_avGet = http_get_begin(G.host, G.port, "/get_avatars", 4000);
}

static void avatars_step(void)
{
	if (!g_avGet) return;
	int r = http_get_step(g_avGet);
	if (r == 0) return;
	char* body = NULL;
	if (r == 1) body = http_get_take_body(g_avGet);
	http_get_close(g_avGet);
	g_avGet = NULL;
	if (!body) return;

	static JsDoc doc;
	if (json_parse(&doc, body)) {
		int arr = json_obj_get(&doc, 0, "avatars");
		int n   = json_arr_len(&doc, arr);
		if (n > 0) {
			if (n > MAX_AVATARS) n = MAX_AVATARS;
			s_nAvatars = n;
			for (int i = 0; i < n; i++) {
				const char* nm = json_str(&doc, json_arr_at(&doc, arr, i), "");
				snprintf(s_avatars[i], sizeof(s_avatars[i]), "%s", nm);
			}
		}
	}
	free(body);
	if (s_nAvatars == 0) avatars_defaults();
}

// ------------------------------------------------------------- game protocol

static void connect_start(void)
{
	net_teardown();
	G.state = ST_CONNECTING;
	G.error[0] = '\0';
	G.myScore  = 0;
	G.nBoard   = 0;
	G.ws = ws_begin(G.host, G.port, SIO_PATH);
	if (!G.ws) set_error("Out of memory");
}

static void send_join(void)
{
	char nameEsc[128], avatarEsc[128], codeEsc[32], args[384];
	json_escape(G.player, nameEsc, sizeof(nameEsc));
	json_escape(G.avatar, avatarEsc, sizeof(avatarEsc));
	json_escape(G.code, codeEsc, sizeof(codeEsc));
	snprintf(args, sizeof(args),
	         "{\"code\":\"%s\",\"name\":\"%s\",\"avatar\":\"%s\"}",
	         codeEsc, nameEsc, avatarEsc);
	sio_emit(G.ws, "join_game", args);
	G.joinSent = true;
}

static void submit_answer(void)
{
	float secs = (float)(osGetTime() - G.qStartMs) / 1000.0f;
	if (secs < 0.0f) secs = 0.0f;

	char args[768];
	if (G.qType == QT_MC) {
		if (G.selected < 0) return;
		snprintf(args, sizeof(args),
		         "{\"code\":\"%s\",\"answer\":%d,\"time_taken\":%.2f}",
		         G.code, G.selected, secs);
		snprintf(G.submittedText, sizeof(G.submittedText), "%s", G.answers[G.selected]);
	} else if (G.qType == QT_SLIDER) {
		snprintf(args, sizeof(args),
		         "{\"code\":\"%s\",\"answer\":%g,\"time_taken\":%.2f}",
		         G.code, G.sValue, secs);
		snprintf(G.submittedText, sizeof(G.submittedText), "%.1f", G.sValue);
	} else if (G.qType == QT_PUZZLE) {
		// Submit the player's order as a JSON array of answer strings.
		char esc[ANS_LEN * 2];
		int o = snprintf(args, sizeof(args), "{\"code\":\"%s\",\"answer\":[", G.code);
		for (int i = 0; i < G.nAnswers && o < (int)sizeof(args) - 40; i++) {
			json_escape(G.answers[G.pOrder[i]], esc, sizeof(esc));
			o += snprintf(args + o, sizeof(args) - o, "%s\"%s\"", i ? "," : "", esc);
		}
		o += snprintf(args + o, sizeof(args) - o, "],\"time_taken\":%.2f}", secs);

		// Arrow chain for the reveal card.
		o = 0;
		for (int i = 0; i < G.nAnswers; i++) {
			o += snprintf(G.submittedText + o, sizeof(G.submittedText) - o,
			              "%s%s", i ? " -> " : "", G.answers[G.pOrder[i]]);
		}
	} else {
		if (!G.typed[0]) return;
		char esc[ANS_LEN * 2];
		json_escape(G.typed, esc, sizeof(esc));
		snprintf(args, sizeof(args),
		         "{\"code\":\"%s\",\"answer\":\"%s\",\"time_taken\":%.2f}",
		         G.code, esc, secs);
		snprintf(G.submittedText, sizeof(G.submittedText), "%s", G.typed);
	}
	sio_emit(G.ws, "submit_answer", args);
	G.state = ST_WAITING;
}

static void read_board(SioMsg* m)
{
	int arr = json_obj_get(&m->doc, m->data, "leaderboard");
	int n   = json_arr_len(&m->doc, arr);
	if (n > MAX_BOARD) n = MAX_BOARD;
	G.nBoard = n;
	for (int i = 0; i < n; i++) {
		int row = json_arr_at(&m->doc, arr, i);
		const char* nm = json_ostr(&m->doc, row, "name", "?");
		snprintf(G.board[i].name, sizeof(G.board[i].name), "%s", nm);
		G.board[i].score = (int)json_onum(&m->doc, row, "score", 0);
		G.board[i].left  = json_obool(&m->doc, row, "left", false);

		int lc = json_obj_get(&m->doc, row, "last_correct");
		if (lc < 0 || m->doc.nodes[lc].type == JS_NULL) G.board[i].lastCorrect = -1;
		else G.board[i].lastCorrect = json_bool(&m->doc, lc, false) ? 1 : 0;

		if (!strcmp(G.board[i].name, G.player)) {
			int prev = G.myScore;
			G.myScore  = G.board[i].score;
			G.lastGain = G.myScore - prev;
		}
	}
	G.boardScroll = 0;
}

static void img_reset(void);
static void img_step(void);
static void img_animate(void);

static HttpGet*  g_imgGet = NULL;
static char      g_imgPath[192];
static bool      g_imgPending = false;  // advertised, not yet shown
static bool      g_imgLoaded  = false;  // texture ready
static u64       g_imgRetryAt = 0;      // don't hammer the server every frame
static int       g_imgTries   = 0;      // diagnostics: attempts on the current image
static char      g_imgDiag[64];         // diagnostics: why the last attempt failed
static int       g_imgW, g_imgH;
static int       g_imgPW, g_imgPH;      // padded texture dims
static C3D_Tex   g_imgTex;
static Tex3DS_SubTexture g_imgSubtex;
static C2D_Image g_imgImage;

// Animated GIF support: the server packs frames into one .565 with a
// u8 frame-count + u8 delay header; the client cycles frames in place.
static u8*  g_imgFrames = NULL;
static int  g_imgFramesN = 0;
static int  g_imgDelay = 0;             // centiseconds per frame
static int  g_imgCur = 0;
static u64  g_imgNextAt = 0;
static bool g_imgIsGif = false;         // the advertised URL ended in .gif
static char* g_imgBodyOwn = NULL;       // owns the adopted frame buffer

// SD card cache: question images live under sdmc:/QuizFabric. Downloads
// stream into downloading/ (resumable), finished files move to
// cachedfiles/<quiz name>/ and are replayed from there.
static bool g_imgUseCache = false;      // question images cache; covers don't
static bool g_imgFromCache = false;     // label says "Loading from cache"
static bool g_imgDlRunning = false;
static int  g_imgDlOffset = 0;          // resume offset (partial file size)
static FILE* g_imgDlF = NULL;
static bool g_imgStreamFile = false;    // animated frames streamed from SD
static FILE* g_imgAnimF = NULL;
static int  g_imgFrameHdr = 8;          // animated header size (8 or legacy 6)
static char g_imgQuiz[96];
static char g_imgCacheFile[320];
static char g_imgDlFile[320];

// Creates a directory if it does not exist (sdmc: paths).
static void img_mkdir(const char* path)
{
	mkdir(path, 0777);
}

static void img_make_dirs(void)
{
	img_mkdir("sdmc:/QuizFabric");
	img_mkdir("sdmc:/QuizFabric/downloading");
	img_mkdir("sdmc:/QuizFabric/cachedfiles");
}

// Deletes every cached file for g_imgQuiz (the quiz was deleted or changed).
static void img_cache_delete_quiz(void)
{
	char dir[320];
	snprintf(dir, sizeof(dir), "sdmc:/QuizFabric/cachedfiles/%s", g_imgQuiz);
	DIR* d = opendir(dir);
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.') continue;
		char path[384];
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		remove(path);
	}
	closedir(d);
	// ctrulib has no rmdir; the empty folder is harmless and reused if the
	// quiz comes back.
}

static void on_event(SioMsg* m)
{
	const char* ev = m->event;

	if (!strcmp(ev, "joined")) {
		const char* qn = json_ostr(&m->doc, m->data, "quiz_name", "Quiz");
		snprintf(G.quizName, sizeof(G.quizName), "%s", qn);
		G.state = ST_LOBBY;
		config_save();
		return;
	}

	if (!strcmp(ev, "error")) {
		const char* msg = json_ostr(&m->doc, m->data, "message", "Server rejected the join");
		set_error("%s", msg);
		net_teardown();
		return;
	}

	if (!strcmp(ev, "kicked")) {
		set_error("%s", json_ostr(&m->doc, m->data, "message",
		                          "You were kicked from the game"));
		net_teardown();
		return;
	}

	if (!strcmp(ev, "new_question")) {
		const char* q = json_ostr(&m->doc, m->data, "question", "");
		snprintf(G.question, sizeof(G.question), "%s", q);

		const char* type = json_ostr(&m->doc, m->data, "type", "type");
		if (!strcmp(type, "mc")) {
			G.qType = QT_MC;
		} else if (!strcmp(type, "slider")) {
			G.qType = QT_SLIDER;
		} else if (!strcmp(type, "puzzle")) {
			G.qType = QT_PUZZLE;
		} else {
			G.qType = QT_TYPE;
		}
		G.isMC = (G.qType == QT_MC);

		G.nAnswers = 0;
		if (G.qType == QT_MC || G.qType == QT_PUZZLE) {
			int arr = json_obj_get(&m->doc, m->data, "answers");
			int n   = json_arr_len(&m->doc, arr);
			if (n > MAX_ANSWERS) n = MAX_ANSWERS;
			for (int i = 0; i < n; i++) {
				const char* a = json_str(&m->doc, json_arr_at(&m->doc, arr, i), "");
				snprintf(G.answers[i], ANS_LEN, "%s", a);
			}
			G.nAnswers = n;
		}

		if (G.qType == QT_SLIDER) {
			G.sMin = (float)json_onum(&m->doc, m->data, "min", 0);
			G.sMax = (float)json_onum(&m->doc, m->data, "max", 100);
			if (G.sMax <= G.sMin) G.sMax = G.sMin + 1.0f;
			G.sValue = (G.sMin + G.sMax) / 2.0f;
		}

		if (G.qType == QT_PUZZLE) {
			G.pSel = 0;
			for (int i = 0; i < MAX_ANSWERS; i++) G.pOrder[i] = i;
		}

		// Question image: remember the URL and download it in the background.
		int imgs = json_obj_get(&m->doc, m->data, "images");
		const char* url = json_arr_len(&m->doc, imgs) > 0
			? json_str(&m->doc, json_arr_at(&m->doc, imgs, 0), "") : "";
		img_request(url[0] ? url : NULL);

		G.selected = -1;
		G.typed[0] = '\0';
		G.submittedText[0] = '\0';
		G.correctAnswer[0] = '\0';
		G.qNumber++;
		G.qStartMs = osGetTime();
		G.state    = ST_QUESTION;
		return;
	}

	if (!strcmp(ev, "answer_revealed")) {
		G.revealCorrect = json_obool(&m->doc, m->data, "correct", false);
		const char* ca  = json_ostr(&m->doc, m->data, "correct_answer", "");
		snprintf(G.correctAnswer, sizeof(G.correctAnswer), "%s", ca);
		G.state = ST_REVEALED;
		G.revealUntil = osGetTime() + 3500;
		return;
	}

	if (!strcmp(ev, "answers_revealed")) {
		const char* ca = json_ostr(&m->doc, m->data, "correct_answer", "");
		if (ca[0] && !G.correctAnswer[0]) {
			snprintf(G.correctAnswer, sizeof(G.correctAnswer), "%s", ca);
		}
		return;
	}

	if (!strcmp(ev, "leaderboard")) {
		const char* ca = json_ostr(&m->doc, m->data, "correct_answer", "");
		if (ca[0]) snprintf(G.correctAnswer, sizeof(G.correctAnswer), "%s", ca);
		read_board(m);
		// Hold the personal correct/wrong card briefly before the standings.
		if (G.state != ST_REVEALED) G.state = ST_LEADERBOARD;
		return;
	}

	if (!strcmp(ev, "final_leaderboard")) {
		read_board(m);
		G.state = ST_FINAL;
		return;
	}

	if (!strcmp(ev, "player_joined")) {
		toast("%s joined", json_ostr(&m->doc, m->data, "name", "A player"));
		return;
	}
	if (!strcmp(ev, "player_left")) {
		toast("%s left", json_ostr(&m->doc, m->data, "name", "A player"));
		return;
	}
	if (!strcmp(ev, "answer_received")) {
		toast("%s answered", json_ostr(&m->doc, m->data, "name", "Someone"));
		return;
	}
	if (!strcmp(ev, "show_popup")) {
		toast("%s", json_ostr(&m->doc, m->data, "message", ""));
		return;
	}
}

static void net_update(void)
{
	if (!G.ws) {
		// Auto-retry timer: the 3DS SOC stack often refuses the first few
		// connection attempts, so retry quietly instead of failing the join.
		if (G.state == ST_CONNECTING && G.retryAt && osGetTime() >= G.retryAt) {
			G.retryAt = 0;
			G.ws = ws_begin(G.host, G.port, SIO_PATH);
			if (!G.ws) set_error("Out of memory");
		}
		return;
	}

	WsState st = ws_step(G.ws);
	if (st == WSS_ERR) {
		G.retries++;
		G.retryAt = osGetTime() + RETRY_DELAY_MS;
		ws_close(G.ws);
		G.ws = NULL;
		G.nsSent = false;
		G.joinSent = false;
		toast("Connection refused - retrying (%d)", G.retries);
		return;
	}
	if (st == WSS_CLOSED) {
		set_error("Disconnected from server");
		net_teardown();
		return;
	}
	if (st != WSS_OPEN) return;

	if (G.retries) {
		G.retries = 0;
		G.retryAt = 0;
	}

	int len = 0;
	int r;
	while ((r = ws_poll(G.ws, g_rx, sizeof(g_rx), &len)) == 1) {
		sio_decode(g_rx, len, &g_msg);
		switch (g_msg.kind) {
		case SIO_OPEN:
			if (!G.nsSent) {
				sio_connect_ns(G.ws);
				G.nsSent = true;
			}
			break;
		case SIO_PING:
			sio_pong(G.ws);
			break;
		case SIO_CONNECTED:
			if (!G.joinSent) send_join();
			break;
		case SIO_CONNECT_ERROR:
			set_error("Server refused the socket.io connection");
			net_teardown();
			return;
		case SIO_DISCONNECTED:
			set_error("Server closed the session");
			net_teardown();
			return;
		case SIO_EVENT:
			on_event(&g_msg);
			break;
		default:
			break;
		}
		if (!G.ws) return; // an event tore the connection down
	}
	if (r < 0 && G.ws) {
		set_error("%s", ws_error(G.ws));
		net_teardown();
	}

	// answer_revealed lands before the room-wide leaderboard, so hold the
	// personal correct/wrong card for a beat, then show the standings.
	if (G.state == ST_REVEALED && G.nBoard > 0 && osGetTime() > G.revealUntil) {
		G.state = ST_LEADERBOARD;
	}
}

// ------------------------------------------------------------ question image
//
// The server advertises a PNG per question; it also serves a pre-converted
// .565 next to it (u16 w, u16 h, then w*h little-endian RGB565). The fetch
// runs in the background so answering is never blocked by the slow Wi-Fi.

static void img_reset(void)
{
	if (g_imgGet) {
		http_get_close(g_imgGet);
		g_imgGet = NULL;
	}
	if (g_imgDlF) {
		fclose(g_imgDlF);
		g_imgDlF = NULL;
	}
	g_imgDlRunning = false;
	g_imgDlOffset = 0;
	if (g_imgAnimF) {
		fclose(g_imgAnimF);
		g_imgAnimF = NULL;
	}
	g_imgStreamFile = false;
	g_imgFromCache = false;
	g_imgUseCache = false;
	g_imgPending = false;
	g_imgRetryAt = 0;
	g_imgTries = 0;
	g_imgIsGif = false;
	// g_imgFrames points INSIDE g_imgBodyOwn (the adopted receive buffer),
	// so only the base pointer may be freed.
	free(g_imgBodyOwn);
	g_imgBodyOwn = NULL;
	g_imgFrames = NULL;
	g_imgFramesN = 0;
	g_imgCur = 0;
	g_imgNextAt = 0;
	if (g_imgLoaded) {
		C3D_TexDelete(&g_imgTex);
		g_imgLoaded = false;
	}
}

// Builds the SD card cache layout for a .565 question-image URL:
//   sdmc:/QuizFabric/cachedfiles/<quiz>/<file>.565
//   sdmc:/QuizFabric/downloading/<quiz>~<file>.565  (partial downloads)
// quizOut receives the decoded, FAT-sanitised quiz name. Returns false when
// the URL is not a /UploadedImages/... image. Also used by the pre-cache
// mode.
bool img_cache_paths(const char* url565, char* quizOut, int quizSz,
                     char* cacheOut, int cacheSz, char* dlOut, int dlSz)
{
	if (!url565 || !quizOut || !cacheOut || !dlOut) return false;
	char* up = strstr(url565, "/UploadedImages/");
	if (!up) return false;
	up += strlen("/UploadedImages/");
	char* slash = strchr(up, '/');
	if (!slash || !slash[1]) return false;

	char quiz[96];
	size_t qlen = (size_t)(slash - up);
	if (qlen >= sizeof(quiz)) qlen = sizeof(quiz) - 1;
	memcpy(quiz, up, qlen);
	quiz[qlen] = '\0';
	// Decode %20 -> space, then sanitise for the FAT filesystem.
	char* w = quiz;
	for (const char* s = quiz; *s; s++) {
		if (s[0] == '%' && s[1] == '2' && s[2] == '0') {
			*w++ = ' ';
			s += 2;
		} else {
			*w++ = *s;
		}
	}
	*w = '\0';
	for (char* c = quiz; *c; c++) {
		if (strchr("\\/:*?\"<>|", *c)) *c = '_';
	}
	char fileSan[128];
	snprintf(fileSan, sizeof(fileSan), "%s", slash + 1);
	for (char* c = fileSan; *c; c++) {
		if (strchr("\\/:*?\"<>|", *c)) *c = '_';
	}
	snprintf(quizOut, quizSz, "%s", quiz);
	snprintf(cacheOut, cacheSz, "sdmc:/QuizFabric/cachedfiles/%s/%s", quiz, fileSan);
	snprintf(dlOut, dlSz, "sdmc:/QuizFabric/downloading/%s~%s", quiz, fileSan);
	return true;
}

// Starts a background fetch of the question image. `url` is the PNG URL from
// the server; the client swaps the extension for the pre-converted .565 copy
// and percent-encodes spaces. NULL clears the image. Also used by solo mode.
void img_request(const char* url)
{
	img_reset();
	G.hasImages = false;
	if (!url || !url[0]) return;

	char tmp[192];
	snprintf(tmp, sizeof(tmp), "%s", url);
	// The advertised URL's extension tells us it is a GIF before the swap
	// to the .565 conversion copy, so the progress label can say so.
	char* dot = strrchr(tmp, '.');
	g_imgIsGif = dot && strcasecmp(dot, ".gif") == 0;
	if (dot) {
		*dot = '\0';
		snprintf(dot, sizeof(tmp) - (size_t)(dot - tmp), ".565");
	}

	g_imgUseCache = img_cache_paths(tmp, g_imgQuiz, sizeof(g_imgQuiz),
	                                g_imgCacheFile, sizeof(g_imgCacheFile),
	                                g_imgDlFile, sizeof(g_imgDlFile));

	char* d = g_imgPath;
	for (const char* s = tmp; *s && d < g_imgPath + sizeof(g_imgPath) - 4; s++) {
		if (*s == ' ') {
			*d++ = '%'; *d++ = '2'; *d++ = '0';
		} else {
			*d++ = *s;
		}
	}
	*d = '\0';
	g_imgPending = true;
	G.hasImages = true;
}

// Same as img_request, but the path is used exactly as given: no .565
// extension swap and no space encoding. Used for the Kahoot cover preview
// proxy, whose query string must survive untouched.
void img_request_raw(const char* path)
{
	img_reset();
	G.hasImages = false;
	if (!path || !path[0]) return;
	snprintf(g_imgPath, sizeof(g_imgPath), "%s", path);
	g_imgPending = true;
	G.hasImages = true;
}

bool img_is_loaded(void) { return g_imgLoaded; }
bool img_is_gif(void) { return g_imgIsGif; }
bool img_pending(void) { return g_imgPending; }
// True while the image is being replayed from the SD card cache (the
// progress label says "Loading from cache" instead of downloading).
bool img_is_from_cache(void) { return g_imgFromCache; }

// Offline mode: when a cached copy is missing, skip the download entirely.
static bool g_imgOffline = false;
void img_set_offline(bool on) { g_imgOffline = on; }

// Download progress: 1 when loaded, -1 unknown, else 0..1.
float img_progress(void)
{
	if (g_imgLoaded) return 1.0f;
	if (!g_imgGet) return -1.0f;
	return http_get_progress(g_imgGet);
}

// Draws the loaded question image centred at cx, fitting maxw x maxh with the
// top edge at topY.
void img_draw_at(float cx, float topY, float maxw, float maxh)
{
	if (!g_imgLoaded) return;
	float s = maxh / (float)g_imgH;
	float sw = (float)g_imgW * s;
	if (sw > maxw) {
		s = maxw / (float)g_imgW;
		sw = maxw;
	}
	float sh = (float)g_imgH * s;
	C2D_DrawImageAt(g_imgImage, cx - sw / 2 + ui_parallax(PAR_IMAGE),
	                topY + (maxh - sh) / 2, 0.5f, NULL, s, s);
}

// Decodes a .565 straight from the SD card. Static images are uploaded and
// the file closed; animated GIFs keep the file open (g_imgAnimF) and stream
// frames per tick from img_animate(). Returns false when the file is not a
// valid .565 (the caller should delete it and re-download).
static bool img_decode_file(FILE* f)
{
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsz < 8) return false;
	u8 head[8];
	if (fread(head, 1, 8, f) != 8) return false;
	u16 w = (u16)(head[0] | (head[1] << 8));
	u16 h = (u16)(head[2] | (head[3] << 8));
	if (w < 1 || h < 1) return false;
	int pw = 8; while (pw < w) pw <<= 1;
	int ph = 8; while (ph < h) ph <<= 1;
	int frameBytes = pw * ph * 4;

	bool anim = false;
	int frames = 0, delay = 10, hdr = 4;
	if (fsz == 4 + frameBytes) {
		// static: 4-byte header
	} else if (fsz == 8 + (int)(head[4] | (head[5] << 8)) * frameBytes) {
		anim = true;
		frames = head[4] | (head[5] << 8);
		delay = head[6] | (head[7] << 8);
		hdr = 8;
		if (frames < 2 || frames > 2048) return false;
	} else if (fsz == 6 + (int)head[4] * frameBytes) {
		// legacy 6-byte animated cached copy
		anim = true;
		frames = head[4];
		delay = head[5];
		hdr = 6;
		if (frames < 2 || frames > 2048) return false;
	} else if (fsz == 7 + (int)head[4] * frameBytes) {
		// intermediate sampled format (u8 count, u16 delay)
		anim = true;
		frames = head[4];
		delay = head[5] | (head[6] << 8);
		hdr = 7;
		if (frames < 2 || frames > 2048) return false;
	} else {
		return false;
	}

	if (!C3D_TexInit(&g_imgTex, (u16)pw, (u16)ph, GPU_RGBA8)) return false;
	C3D_TexSetFilter(&g_imgTex, GPU_LINEAR, GPU_NEAREST);
	C3D_TexSetWrap(&g_imgTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	g_imgSubtex = (Tex3DS_SubTexture){ w, h, 0.0f, 1.0f,
	                                  (float)w / (float)pw, 1.0f - (float)h / (float)ph };
	g_imgImage = (C2D_Image){ &g_imgTex, &g_imgSubtex };
	g_imgW = w;
	g_imgH = h;
	g_imgPW = pw;
	g_imgPH = ph;

	if (anim) {
		g_imgFrames = NULL;
		g_imgFramesN = frames;
		g_imgDelay = delay > 0 ? delay : 10;
		g_imgCur = 0;
		g_imgNextAt = osGetTime() + (u64)g_imgDelay * 10;
		g_imgFrameHdr = hdr;
		g_imgStreamFile = true;
		g_imgAnimF = f; // keep open; img_reset closes it
	}
	// Upload the first frame (static: the only frame).
	void* lin = linearAlloc((size_t)frameBytes);
	if (lin) {
		fseek(f, hdr, SEEK_SET);
		fread(lin, 1, (size_t)frameBytes, f);
		C3D_TexUpload(&g_imgTex, lin);
		C3D_TexFlush(&g_imgTex);
		linearFree(lin);
	}
	return true;
}

// Advances the SD-cache pipeline for question images: load from cache when
// the file exists, otherwise download to downloading/ (resuming partials)
// and move the finished file into cachedfiles/<quiz>/.
static void img_step_cache(void)
{
	if (g_imgDlRunning) {
		int r = http_get_step(g_imgGet);
		if (r == 0) return;
		int status = http_get_status(g_imgGet);
		int downloaded = http_get_downloaded(g_imgGet);
		http_get_close(g_imgGet);
		g_imgGet = NULL;
		fclose(g_imgDlF);
		g_imgDlF = NULL;
		g_imgDlRunning = false;
		if (status == 404) {
			// The quiz (or its image) was deleted: drop the partial file and
			// every cached copy for this quiz.
			remove(g_imgDlFile);
			img_cache_delete_quiz();
			G.hasImages = false;
			g_imgPending = false;
			return;
		}
		if (r == 1) {
			// Complete: move into the cache and replay from there.
			img_make_dirs();
			char dir[320];
			snprintf(dir, sizeof(dir), "sdmc:/QuizFabric/cachedfiles/%s", g_imgQuiz);
			img_mkdir(dir);
			remove(g_imgCacheFile); // replace any older copy
			rename(g_imgDlFile, g_imgCacheFile);
			g_imgDlOffset = 0;
			return; // next frame: load from the cache
		}
		// Interrupted: the partial file stays on disk; resume from its size.
		g_imgDlOffset = downloaded;
		g_imgTries++;
		g_imgRetryAt = osGetTime() + 1000;
		return;
	}

	// Check the cache first.
	FILE* f = fopen(g_imgCacheFile, "rb");
	if (!f) {
		// Offline: no network, so missing cache means no image.
		if (g_imgOffline) {
			G.hasImages = false;
			g_imgPending = false;
			return;
		}
		// Static images are small: download them into RAM like before (no
		// SD-write hitches mid-download), then stash the cache copy in one
		// write. GIFs stream to the SD card so a long animation never has
		// to fit in RAM.
		if (!g_imgIsGif) {
			if (!g_imgGet) {
				g_imgGet = http_get_begin(G.host, G.port, g_imgPath, 20000);
				if (!g_imgGet) {
					g_imgTries++;
					snprintf(g_imgDiag, sizeof(g_imgDiag), "begin: %s",
					         http_begin_error());
					g_imgRetryAt = osGetTime() + 1000;
				}
				return;
			}
			int r = http_get_step(g_imgGet);
			if (r == 0) return;
			int len = 0;
			char* body = (r == 1) ? http_get_take_body_len(g_imgGet, &len) : NULL;
			http_get_close(g_imgGet);
			g_imgGet = NULL;
			if (!body || len < 8) {
				free(body);
				g_imgTries++;
				g_imgRetryAt = osGetTime() + 1000;
				return;
			}
			u16 w = (u16)((unsigned char)body[0] | ((unsigned char)body[1] << 8));
			u16 h = (u16)((unsigned char)body[2] | ((unsigned char)body[3] << 8));
			int pw = 8; while (pw < w) pw <<= 1;
			int ph = 8; while (ph < h) ph <<= 1;
			if (w < 1 || h < 1 || len != 4 + pw * ph * 4) {
				free(body);
				g_imgTries++;
				g_imgRetryAt = osGetTime() + 1000;
				return;
			}
			if (!C3D_TexInit(&g_imgTex, (u16)pw, (u16)ph, GPU_RGBA8)) {
				free(body);
				g_imgTries++;
				g_imgRetryAt = osGetTime() + 1000;
				return;
			}
			C3D_TexSetFilter(&g_imgTex, GPU_LINEAR, GPU_NEAREST);
			C3D_TexSetWrap(&g_imgTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
			g_imgSubtex = (Tex3DS_SubTexture){ w, h, 0.0f, 1.0f,
			                                  (float)w / (float)pw, 1.0f - (float)h / (float)ph };
			g_imgImage = (C2D_Image){ &g_imgTex, &g_imgSubtex };
			g_imgW = w;
			g_imgH = h;
			g_imgPW = pw;
			g_imgPH = ph;
			C3D_TexUpload(&g_imgTex, body + 4);
			C3D_TexFlush(&g_imgTex);
			// One cache write for next time.
			img_make_dirs();
			char dir[320];
			snprintf(dir, sizeof(dir), "sdmc:/QuizFabric/cachedfiles/%s", g_imgQuiz);
			img_mkdir(dir);
			FILE* cf = fopen(g_imgCacheFile, "wb");
			if (cf) {
				setvbuf(cf, NULL, _IOFBF, 32768);
				fwrite(body, 1, (size_t)len, cf);
				fclose(cf);
			}
			free(body);
			g_imgLoaded = true;
			g_imgPending = false;
			return;
		}

		// Start (or resume) the download.
		img_make_dirs();
		FILE* dl = fopen(g_imgDlFile, "r+b");
		if (!dl) dl = fopen(g_imgDlFile, "wb");
		if (!dl) {
			g_imgTries++;
			g_imgRetryAt = osGetTime() + 1000;
			return;
		}
		fseek(dl, 0, SEEK_END);
		g_imgDlOffset = (int)ftell(dl);
		// Small buffered writes keep each frame's SD work tiny and smooth.
		setvbuf(dl, NULL, _IOFBF, 16384);
		g_imgDlF = dl;
		g_imgDlRunning = true;
		g_imgGet = http_get_begin_file(G.host, G.port, g_imgPath, 20000,
		                               dl, g_imgDlOffset, g_imgDlFile);
		if (!g_imgGet) {
			fclose(g_imgDlF);
			g_imgDlF = NULL;
			g_imgDlRunning = false;
			g_imgTries++;
			snprintf(g_imgDiag, sizeof(g_imgDiag), "begin: %s",
			         http_begin_error());
			g_imgRetryAt = osGetTime() + 1000;
		}
		return;
	}

	// Cache hit: decode straight from the SD card.
	g_imgFromCache = true;
	if (!img_decode_file(f)) {
		// Corrupt cached copy: drop it and fall back to downloading.
		fclose(f);
		remove(g_imgCacheFile);
		g_imgFromCache = false;
		return;
	}
	if (g_imgAnimF != f) fclose(f); // static images are fully read
	g_imgLoaded = true;
	g_imgPending = false;
}

// Advances the background download; call once per frame while a question with
// an image is on screen.
static void img_step(void)
{
	img_animate(); // keep cycling loaded GIF frames
	if (!g_imgPending || g_imgLoaded) return;
	if (osGetTime() < g_imgRetryAt) return; // wait out the retry gap
	if (g_imgUseCache) {
		img_step_cache();
		return;
	}

	if (!g_imgGet) {
		// 20s stall timeout (not a total limit): multi-megabyte GIFs need
		// time to trickle in over the 3DS Wi-Fi without aborting.
		g_imgGet = http_get_begin(G.host, G.port, g_imgPath, 20000);
		if (!g_imgGet) {
			g_imgTries++;
			snprintf(g_imgDiag, sizeof(g_imgDiag), "begin: %s",
			         http_begin_error());
			g_imgRetryAt = osGetTime() + 1000; // refused; try again in 1s
			return;
		}
	}
	int r = http_get_step(g_imgGet);
	if (r == 0) return;
	if (r < 0) {
		snprintf(g_imgDiag, sizeof(g_imgDiag), "step: %s",
		         http_get_stage(g_imgGet));
		http_get_close(g_imgGet);
		g_imgGet = NULL;
		g_imgTries++;
		g_imgRetryAt = osGetTime() + 1000;
		return;
	}

	int len = 0;
	// take_body_keep hands over the receive buffer WITHOUT copying, so a
	// many-megabyte animated GIF never exists twice in RAM.
	char* body = http_get_take_body_keep(g_imgGet, &len);
	http_get_close(g_imgGet);
	g_imgGet = NULL;
	if (!body || len < 4) {
		free(body);
		g_imgRetryAt = osGetTime() + 1000;
		return;
	}

	u16 w = (u16)((unsigned char)body[0] | ((unsigned char)body[1] << 8));
	u16 h = (u16)((unsigned char)body[2] | ((unsigned char)body[3] << 8));
	if (w < 1 || h < 1) {
		free(body);
		g_imgRetryAt = osGetTime() + 1000; // not an image; don't loop hard
		return;
	}

	// The payload is RGBA8 already swizzled into the PICA200's native tiled
	// layout (8x8 Morton blocks), padded to power-of-two with white by the
	// server. C3D_TexUpload is a plain memcpy, so the bytes must arrive
	// exactly as the GPU expects them. Animated GIFs add a u8 frame count and
	// u8 delay header before the frame payloads.
	int pw = 8; while (pw < w) pw <<= 1;
	int ph = 8; while (ph < h) ph <<= 1;
	g_imgPW = pw;
	g_imgPH = ph;
	int frameBytes = pw * ph * 4;

	// Animated question images come in two flavours: the current format
	// (u16 frame count + u16 delay - EVERY frame of the GIF is kept) and
	// the legacy u8 format some cached .565 copies still use.
	int nf = (unsigned char)body[4] | ((unsigned char)body[5] << 8);
	bool animNew = (len == 8 + nf * frameBytes);
	bool animOld = (len == 6 + (int)(unsigned char)body[4] * frameBytes);
	bool animMid = (len == 7 + (int)(unsigned char)body[4] * frameBytes);
	if (len != 4 + frameBytes && !animNew && !animOld && !animMid) {
		free(body);
		g_imgRetryAt = osGetTime() + 1000;
		return;
	}

	if (!C3D_TexInit(&g_imgTex, (u16)pw, (u16)ph, GPU_RGBA8)) {
		free(body);
		g_imgRetryAt = osGetTime() + 1000;
		return;
	}
	C3D_TexSetFilter(&g_imgTex, GPU_LINEAR, GPU_NEAREST);
	C3D_TexSetWrap(&g_imgTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

	g_imgSubtex = (Tex3DS_SubTexture){
		w, h, 0.0f, 1.0f, (float)w / (float)pw, 1.0f - (float)h / (float)ph };
	g_imgImage = (C2D_Image){ &g_imgTex, &g_imgSubtex };
	g_imgW = w;
	g_imgH = h;

	// Animated frames: keep them and cycle in img_animate().
	free(g_imgBodyOwn);
	g_imgBodyOwn = NULL;
	g_imgFrames = NULL;
	g_imgFramesN = 0;
	bool adopted = false;
	if (animNew || animOld || animMid) {
		int frames = animNew ? nf : (unsigned char)body[4];
		int delay  = animNew ? ((unsigned char)body[6] | ((unsigned char)body[7] << 8))
		                     : (animMid ? ((unsigned char)body[5] | ((unsigned char)body[6] << 8))
		                                : (unsigned char)body[5]);
		const u8* framesAt = (const u8*)body + (animNew ? 8 : (animMid ? 7 : 6));
		size_t total = (size_t)frames * (size_t)frameBytes;
		if (frames >= 2 && frames <= 2048 && total <= 32 * 1024 * 1024) {
			// Adopt the receive buffer as the frame store: no copy, so the
			// whole GIF lives in RAM exactly once.
			g_imgBodyOwn = body;
			g_imgFrames = (u8*)framesAt;
			g_imgFramesN = frames;
			g_imgDelay = delay > 0 ? delay : 10;
			g_imgCur = 0;
			g_imgNextAt = osGetTime() + (u64)g_imgDelay * 10;
			adopted = true;
		}
	}

	const u8* first = (len == 4 + frameBytes) ? (const u8*)body + 4
	                                          : (const u8*)body + (animNew ? 8 : (animMid ? 7 : 6));
	C3D_TexUpload(&g_imgTex, first);
	C3D_TexFlush(&g_imgTex);

	g_imgLoaded = true;
	g_imgPending = false;
	if (!adopted) free(body);
}

// Cycles animated GIF frames in place (the texture is re-uploaded per frame).
static void img_animate(void)
{
	if (g_imgFramesN < 2) return;
	if (osGetTime() < g_imgNextAt) return;
	g_imgCur = (g_imgCur + 1) % g_imgFramesN;
	g_imgNextAt = osGetTime() + (u64)g_imgDelay * 10;

	if (g_imgStreamFile && g_imgAnimF) {
		// Frames are streamed from the SD card cache: read one frame per
		// tick, so a long GIF never lives in RAM.
		size_t frameBytes = (size_t)g_imgPW * g_imgPH * 4;
		void* lin = linearAlloc(frameBytes);
		if (!lin) return;
		fseek(g_imgAnimF, (long)g_imgFrameHdr + (long)g_imgCur * (long)frameBytes,
		      SEEK_SET);
		if (fread(lin, 1, frameBytes, g_imgAnimF) == frameBytes) {
			C3D_TexUpload(&g_imgTex, lin);
			C3D_TexFlush(&g_imgTex);
		}
		linearFree(lin);
		return;
	}

	// Upload straight from the frame store (C3D_TexUpload is a memcpy).
	C3D_TexUpload(&g_imgTex,
	              g_imgFrames + (size_t)g_imgCur * g_imgPW * g_imgPH * 4);
	C3D_TexFlush(&g_imgTex);
}

// ------------------------------------------------------------------ top draw

static void draw_header(const char* left, const char* right)
{
	ui_panel(0, 0, TOP_W, 26, RGBA(40, 8, 60, 80));
	ui_text(left, 10, 4, 0.52f, CLR_SOFT);
	if (right && *right) ui_text_right(right, TOP_W - 10, 4, 0.52f, CLR_WHITE);
}

static void draw_top(void)
{
	char lines[UI_MAX_LINES][UI_LINE_MAX];
	char buf[128];

	switch (G.state) {
	case ST_MENU: {
		ui_text_center_shadow("QUIZFABRIC", TOP_W / 2, 44, 1.15f, CLR_WHITE);
		ui_text_center("3 D S   C L I E N T", TOP_W / 2, 84, 0.5f, CLR_SOFT);

		ui_panel_hi(60, 118, TOP_W - 120, 84, CLR_GLASS);
		snprintf(buf, sizeof(buf), "%s:%d", G.host, G.port);
		ui_text_center(buf, TOP_W / 2, 128, 0.48f, CLR_WHITE);
		snprintf(buf, sizeof(buf), "%s  as  %s", G.avatar, G.player);
		ui_text_center(buf, TOP_W / 2, 150, 0.48f, CLR_SOFT);
		if (G.code[0]) snprintf(buf, sizeof(buf), "GAME PIN  %s", G.code);
		else           snprintf(buf, sizeof(buf), "NO GAME PIN SET");
		ui_text_center(buf, TOP_W / 2, 174, 0.56f, CLR_GOLD);

		if (G.offline) {
			if (net_ping_state() == 1) {
				ui_text_center("Checking connection...", TOP_W / 2, 204, 0.45f, CLR_SOFT);
			} else {
				ui_text_center("OFFLINE MODE - no internet found", TOP_W / 2, 204, 0.45f, CLR_GOLD);
			}
			ui_text_center("Y = check connection & go online", TOP_W / 2, 222, 0.36f, CLR_DIM);
		} else {
			ui_text_center("Set things up on the touch screen", TOP_W / 2, 210, 0.42f, CLR_DIM);
			ui_text_center("Y = offline mode", TOP_W / 2, 226, 0.36f, CLR_DIM);
		}

#ifdef PROD_BUILD
		snprintf(buf, sizeof(buf), "PRODUCTION build %s", APP_BUILD);
#else
		snprintf(buf, sizeof(buf), "build %s", APP_BUILD);
#endif
		ui_text(buf, 10, 222, 0.4f, CLR_DIM);
		break;
	}

	case ST_CONNECTING: {
		ui_text_center_shadow("CONNECTING", TOP_W / 2, 90, 0.9f, CLR_WHITE);
		snprintf(buf, sizeof(buf), "%s:%d", G.host, G.port);
		ui_text_center(buf, TOP_W / 2, 126, 0.5f, CLR_SOFT);
		float pct = (G.time * 0.4f) - (float)(int)(G.time * 0.4f);
		ui_bar(120, 160, TOP_W - 240, 6, pct, CLR_WHITE, RGBA(255, 255, 255, 60));
		break;
	}

	case ST_LOBBY: {
		draw_header(G.quizName, G.player);
		ui_text_center_shadow("YOU'RE IN!", TOP_W / 2, 72, 0.95f, CLR_WHITE);
		ui_text_center("Waiting for the host to start", TOP_W / 2, 118, 0.52f, CLR_SOFT);
		snprintf(buf, sizeof(buf), "PIN %s", G.code);
		ui_text_center(buf, TOP_W / 2, 150, 0.6f, CLR_GOLD);
		float d = sinf(G.time * 3.0f) * 0.5f + 0.5f;
		float px = ui_parallax(PAR_PANEL);
		C2D_DrawCircleSolid(TOP_W / 2 - 22 + px, 194, 0.5f, 5, ui_fade(CLR_WHITE, 0.35f + d * 0.65f));
		C2D_DrawCircleSolid(TOP_W / 2 + px,      194, 0.5f, 5, ui_fade(CLR_WHITE, 0.35f + (1 - d) * 0.65f));
		C2D_DrawCircleSolid(TOP_W / 2 + 22 + px, 194, 0.5f, 5, ui_fade(CLR_WHITE, 0.35f + d * 0.65f));
		break;
	}

	case ST_QUESTION:
	case ST_WAITING: {
		snprintf(buf, sizeof(buf), "Q%d  %s", G.qNumber, G.quizName);
		char scoreBuf[48];
		snprintf(scoreBuf, sizeof(scoreBuf), "%d pts", G.myScore);
		draw_header(buf, scoreBuf);

		// With an image, the question text band shrinks to leave a 72px
		// picture strip below it.
		float qh = G.hasImages ? 88.0f : 128.0f;
		float scale = 0.8f;
		int n = ui_wrap_fit(G.question, TOP_W - 48, qh, 0.85f, 0.42f,
		                    lines, UI_MAX_LINES, &scale);
		float lh = ui_line_height(scale);
		float y  = 38 + (qh - n * lh) / 2;
		for (int i = 0; i < n; i++) {
			ui_text_center_shadow(lines[i], TOP_W / 2, y + i * lh, scale, CLR_WHITE);
		}

		if (G.hasImages) {
			if (g_imgLoaded) {
				img_draw_at(TOP_W / 2, 128, 256, 72);
			} else {
				// Downloading in the background; the touch screen still works.
				snprintf(buf, sizeof(buf), "%s (try %d)  [%s]",
				         img_is_from_cache() ? "LOADING FROM CACHE"
				         : (img_is_gif() ? "DOWNLOADING GIF" : "DOWNLOADING IMAGE"),
				         g_imgTries + 1, APP_BUILD);
				ui_text_center(buf, TOP_W / 2, 150, 0.42f, CLR_SOFT);
				float p = img_progress();
				if (p >= 0.0f) {
					ui_bar(120, 166, TOP_W - 240, 10, p, CLR_WHITE,
					       RGBA(255, 255, 255, 60));
					snprintf(buf, sizeof(buf), "%d%%", (int)(p * 100.0f));
					ui_text_center(buf, TOP_W / 2, 184, 0.45f, CLR_SOFT);
				} else {
					// Length unknown (still connecting): animated strip.
					float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
					ui_bar(120, 166, TOP_W - 240, 10, pct, CLR_WHITE,
					       RGBA(255, 255, 255, 60));
					ui_text_center("connecting...", TOP_W / 2, 184, 0.42f, CLR_DIM);
				}
				if (g_imgDiag[0]) {
					snprintf(buf, sizeof(buf), "last: %s", g_imgDiag);
					ui_text_center(buf, TOP_W / 2, 198, 0.36f, CLR_DIM);
				}
			}
		}

		if (G.state == ST_WAITING) {
			ui_text_center("Answer locked in", TOP_W / 2, 206, 0.5f, CLR_GOOD);
		} else {
			// Points decay to zero at 20s; show the window draining.
			float secs = (float)(osGetTime() - G.qStartMs) / 1000.0f;
			float pct  = 1.0f - secs / POINT_WINDOW_SEC;
			if (pct < 0.0f) pct = 0.0f;
			int pts = (int)(1000.0f - secs * 50.0f);
			if (pts < 0) pts = 0;
			ui_bar(40, 208, TOP_W - 80, 8,
			       pct, ui_lerp(CLR_BAD, CLR_GOOD, pct), RGBA(255, 255, 255, 55));
			snprintf(buf, sizeof(buf), "%d points available", pts);
			ui_text_center(buf, TOP_W / 2, 220, 0.4f, CLR_SOFT);
		}
		break;
	}

	case ST_REVEALED: {
		u32 accent = G.revealCorrect ? CLR_GOOD : CLR_BAD;
		ui_panel(0, 56, TOP_W, 72, ui_fade(accent, 0.30f));
		ui_text_center_shadow(G.revealCorrect ? "CORRECT!" : "NOT QUITE",
		                      TOP_W / 2, 70, 1.1f, CLR_WHITE);

		if (G.correctAnswer[0]) {
			ui_text_center("ANSWER", TOP_W / 2, 140, 0.42f, CLR_DIM);
			float scale = 0.7f;
			int n = ui_wrap_fit(G.correctAnswer, TOP_W - 60, 54, 0.75f, 0.4f,
			                    lines, 3, &scale);
			for (int i = 0; i < n; i++) {
				ui_text_center(lines[i], TOP_W / 2, 158 + i * ui_line_height(scale),
				               scale, CLR_GOLD);
			}
		}
		if (G.revealCorrect && G.lastGain > 0) {
			snprintf(buf, sizeof(buf), "+%d", G.lastGain);
			ui_text_center_shadow(buf, TOP_W / 2, 108, 0.65f, CLR_GOLD);
		}
		break;
	}

	case ST_LEADERBOARD:
	case ST_FINAL: {
		const bool final = (G.state == ST_FINAL);
		ui_text_center_shadow(final ? "FINAL RESULTS" : "LEADERBOARD",
		                      TOP_W / 2, 22, 0.85f, CLR_WHITE);

		if (!final && G.correctAnswer[0]) {
			snprintf(buf, sizeof(buf), "Answer: %s", G.correctAnswer);
			ui_text_center(buf, TOP_W / 2, 58, 0.45f, CLR_GOLD);
		}

		if (final && G.nBoard > 0) {
			ui_text_center("WINNER", TOP_W / 2, 72, 0.45f, CLR_DIM);
			ui_text_center_shadow(G.board[0].name, TOP_W / 2, 90, 0.85f, CLR_GOLD);
			snprintf(buf, sizeof(buf), "%d points", G.board[0].score);
			ui_text_center(buf, TOP_W / 2, 126, 0.5f, CLR_WHITE);
		}

		// Where the player currently stands.
		int rank = -1;
		for (int i = 0; i < G.nBoard; i++) {
			if (!strcmp(G.board[i].name, G.player)) { rank = i + 1; break; }
		}
		if (rank > 0) {
			snprintf(buf, sizeof(buf), "You are #%d with %d points", rank, G.myScore);
			ui_text_center(buf, TOP_W / 2, final ? 168 : 96, 0.55f, CLR_WHITE);
		}
		ui_text_center(final ? "Thanks for playing!" : "Standings on the touch screen",
		               TOP_W / 2, 214, 0.42f, CLR_DIM);
		break;
	}

	case ST_CONTROL:
	case ST_SOLO:
	case ST_HOST:
	case ST_STEAL:
	case ST_DELETE:
		break;

	case ST_JOIN_SETUP: {
		ui_text_center_shadow("JOIN A GAME", TOP_W / 2, 60, 0.95f, CLR_WHITE);
		snprintf(buf, sizeof(buf), "%s:%d", G.host, G.port);
		ui_text_center(buf, TOP_W / 2, 108, 0.5f, CLR_SOFT);
		snprintf(buf, sizeof(buf), "%s  as  %s", G.avatar, G.player);
		ui_text_center(buf, TOP_W / 2, 134, 0.48f, CLR_SOFT);
		if (G.code[0]) snprintf(buf, sizeof(buf), "GAME PIN  %s", G.code);
		else           snprintf(buf, sizeof(buf), "NO GAME PIN SET");
		ui_text_center(buf, TOP_W / 2, 162, 0.56f, CLR_GOLD);
		ui_text_center("Name, PIN and avatar are on the touch screen",
		               TOP_W / 2, 210, 0.42f, CLR_DIM);
		break;
	}

	case ST_ERROR: {
		ui_panel(0, 50, TOP_W, 112, RGBA(120, 20, 50, 130));
		ui_text_center_shadow("AN ERROR HAS OCCURRED", TOP_W / 2, 66, 0.75f, CLR_WHITE);
		ui_text_center("Reason:", TOP_W / 2, 102, 0.42f, CLR_DIM);
		const char* reason = G.error[0] ? G.error : "Unknown";
		float scale = 0.55f;
		int n = ui_wrap_fit(reason, TOP_W - 60, 48, 0.6f, 0.36f, lines, 3, &scale);
		for (int i = 0; i < n; i++) {
			ui_text_center(lines[i], TOP_W / 2, 120 + i * ui_line_height(scale),
			               scale, CLR_WHITE);
		}
		ui_text_center(G.errorRetry ? "Tap RETRY or BACK below" : "Tap BACK below",
		               TOP_W / 2, 182, 0.45f, CLR_SOFT);
		break;
	}
	}

	// Toast overlay, drawn last so it sits above everything. 208+22 = 230,
	// so it stays fully inside the 240-tall top screen.
	if (G.toast[0] && osGetTime() < G.toastUntil) {
		// Toasts live at the very top of the top screen.
		float w = ui_text_width(G.toast, 0.45f) + 24;
		if (w > TOP_W - 20) w = TOP_W - 20;
		ui_panel(TOP_W / 2 - w / 2, 6, w, 22, RGBA(30, 6, 50, 190));
		ui_text_center(G.toast, TOP_W / 2, 11, 0.45f, CLR_WHITE);
	}
}

// --------------------------------------------------------------- bottom draw

typedef struct { float x, y, w, h; } Rect;

static Rect s_menuServer;
static Rect s_menuBtn[4];   // JOIN / HOST / CONTROL / SOLO
static Rect s_menuSteal, s_menuDelete;
static Rect s_joinRow[3];   // NAME / GAME PIN / AVATAR
static Rect s_joinStart;
static Rect s_submitRect;
static Rect s_tileRect[MAX_ANSWERS];
static Rect s_typeBox;
static Rect s_errRetry, s_errBack;

static void layout_menu(void)
{
	// One server row, a 2x2 grid of solid buttons, then a full-width steal bar.
	s_menuServer.x = 12; s_menuServer.y = 26; s_menuServer.w = BOT_W - 24; s_menuServer.h = 32;

	const float bw = 145.0f, bh = 42.0f, gx = 8.0f, gy = 8.0f;
#ifdef PROD_BUILD
	// No server row: the grid moves up into the freed space.
	s_menuBtn[0].x = 11;      s_menuBtn[0].y = 34;                 // JOIN
	s_menuBtn[1].x = 11 + bw + gx; s_menuBtn[1].y = 34;            // HOST
	s_menuBtn[2].x = 11;      s_menuBtn[2].y = 34 + bh + gy;       // CONTROL
	s_menuBtn[3].x = 11 + bw + gx; s_menuBtn[3].y = 34 + bh + gy;  // SOLO
#else
	s_menuBtn[0].x = 11;      s_menuBtn[0].y = 64;                 // JOIN
	s_menuBtn[1].x = 11 + bw + gx; s_menuBtn[1].y = 64;            // HOST
	s_menuBtn[2].x = 11;      s_menuBtn[2].y = 64 + bh + gy;       // CONTROL
	s_menuBtn[3].x = 11 + bw + gx; s_menuBtn[3].y = 64 + bh + gy;  // SOLO
#endif
	for (int i = 0; i < 4; i++) {
		s_menuBtn[i].w = bw;
		s_menuBtn[i].h = bh;
	}

	s_menuSteal.x = 11;
#ifdef PROD_BUILD
	s_menuSteal.y = 138;
#else
	s_menuSteal.y = 168;
#endif
	s_menuSteal.w = bw;
	s_menuSteal.h = 38;

	s_menuDelete.x = 11 + bw + gx;
#ifdef PROD_BUILD
	s_menuDelete.y = 138;
#else
	s_menuDelete.y = 168;
#endif
	s_menuDelete.w = bw;
	s_menuDelete.h = 38;
}

static void layout_join(void)
{
	for (int i = 0; i < 3; i++) {
		s_joinRow[i].x = 12;
		s_joinRow[i].y = 34 + i * 40.0f;
		s_joinRow[i].w = BOT_W - 24;
		s_joinRow[i].h = 34;
	}
	s_joinStart.x = 12;
	s_joinStart.y = 180;
	s_joinStart.w = BOT_W - 24;
	s_joinStart.h = 46;
}

static void layout_answers(void)
{
	const float top = 8.0f, bottom = 186.0f;
	int n = G.nAnswers;
	if (n <= 0) return;

	if (n <= 3) {
		float h = (bottom - top - (n - 1) * 8.0f) / n;
		if (h > 56.0f) h = 56.0f;
		for (int i = 0; i < n; i++) {
			s_tileRect[i].x = 10;
			s_tileRect[i].y = top + i * (h + 8.0f);
			s_tileRect[i].w = BOT_W - 20;
			s_tileRect[i].h = h;
		}
	} else {
		int rows = (n + 1) / 2;
		float h = (bottom - top - (rows - 1) * 7.0f) / rows;
		if (h > 52.0f) h = 52.0f;
		for (int i = 0; i < n; i++) {
			int r = i / 2, c = i % 2;
			s_tileRect[i].x = 10 + c * 155.0f;
			s_tileRect[i].y = top + r * (h + 7.0f);
			s_tileRect[i].w = 145;
			s_tileRect[i].h = h;
		}
	}
	s_submitRect.x = 10;
	s_submitRect.y = 194;
	s_submitRect.w = BOT_W - 20;
	s_submitRect.h = 38;
}

static void layout_type(void)
{
	s_typeBox.x = 16;
	s_typeBox.y = 46;
	s_typeBox.w = BOT_W - 32;
	s_typeBox.h = 92;

	s_submitRect.x = 10;
	s_submitRect.y = 194;
	s_submitRect.w = BOT_W - 20;
	s_submitRect.h = 38;
}

static Rect s_sliderTrack;
static Rect s_pzRow[MAX_ANSWERS], s_pzUp[MAX_ANSWERS], s_pzDn[MAX_ANSWERS];

static void layout_slider(void)
{
	s_sliderTrack.x = 30;
	s_sliderTrack.y = 112;
	s_sliderTrack.w = BOT_W - 60;
	s_sliderTrack.h = 36;

	s_submitRect.x = 10;
	s_submitRect.y = 194;
	s_submitRect.w = BOT_W - 20;
	s_submitRect.h = 38;
}

static void layout_puzzle(void)
{
	int n = G.nAnswers;
	if (n <= 0) n = 1;
	float gap = 5.0f, top = 14.0f;
	float rowH = (170.0f - (n - 1) * gap) / n;
	if (rowH > 34.0f) rowH = 34.0f;
	for (int i = 0; i < G.nAnswers; i++) {
		float y = top + i * (rowH + gap);
		s_pzRow[i].x = 56;               s_pzRow[i].y = y;
		s_pzRow[i].w = BOT_W - 112;      s_pzRow[i].h = rowH;
		s_pzUp[i].x = 16;                s_pzUp[i].y = y;
		s_pzUp[i].w = 34;                s_pzUp[i].h = rowH;
		s_pzDn[i].x = BOT_W - 50;        s_pzDn[i].y = y;
		s_pzDn[i].w = 34;                s_pzDn[i].h = rowH;
	}
	s_submitRect.x = 10;
	s_submitRect.y = 194;
	s_submitRect.w = BOT_W - 20;
	s_submitRect.h = 38;
}

static void draw_row(Rect r, const char* label, const char* value)
{
	ui_panel_hi(r.x, r.y, r.w, r.h, CLR_GLASS);
	ui_text(label, r.x + 12, r.y + 9, 0.45f, CLR_DIM);
	ui_text_right(value, r.x + r.w - 12, r.y + 8, 0.5f, CLR_WHITE);
}

// Solid chunky menu button, website-style (solid colour + darker bottom edge).
static void draw_menu_btn(int i, const char* label, u32 color, u32 textColor)
{
	Rect r = s_menuBtn[i];
	ui_tile(r.x, r.y, r.w, r.h, color, false);
	ui_text_center(label, r.x + r.w / 2, r.y + 15, 0.6f, textColor);
}

static void draw_bottom(void)
{
	char buf[160];
	char lines[UI_MAX_LINES][UI_LINE_MAX];

	switch (G.state) {
	case ST_MENU: {
		layout_menu();
		if (G.offline) {
			ui_text("SETUP - OFFLINE MODE", 14, 8, 0.5f, CLR_SOFT);
			ui_text_right("START = quit", BOT_W - 14, 10, 0.38f, CLR_DIM);

			ui_tile(20, 60, BOT_W - 40, 60, RGB(255, 208, 92), false);
			ui_text_center("SOLO", BOT_W / 2, 78, 0.7f, CLR_INK);
			ui_text_center("play cached quizzes", BOT_W / 2, 100, 0.42f, CLR_INK);

			ui_tile(20, 130, BOT_W - 40, 60, RGB(255, 92, 116), false);
			ui_text_center("DELETE", BOT_W / 2, 148, 0.7f, CLR_WHITE);
			ui_text_center("delete cached quizzes", BOT_W / 2, 170, 0.42f, CLR_WHITE);

			ui_text_center("Y = check connection & go online", BOT_W / 2, 216, 0.38f, CLR_DIM);
			break;
		}
		ui_text("SETUP", 14, 8, 0.5f, CLR_SOFT);
		ui_text_right("START = quit", BOT_W - 14, 10, 0.38f, CLR_DIM);

#ifndef PROD_BUILD
		snprintf(buf, sizeof(buf), "%s:%d", G.host, G.port);
		draw_row(s_menuServer, "SERVER", buf);
#endif

		draw_menu_btn(0, "JOIN", RGB(56, 214, 160), CLR_INK);
		draw_menu_btn(1, "HOST", RGB(124, 77, 255), CLR_WHITE);
		draw_menu_btn(2, "CONTROL", RGB(104, 132, 255), CLR_WHITE);
		draw_menu_btn(3, "SOLO", RGB(255, 208, 92), CLR_INK);

		ui_tile(s_menuSteal.x, s_menuSteal.y, s_menuSteal.w, s_menuSteal.h,
		        RGB(255, 110, 199), false);
		ui_text_center("KAHOOT", s_menuSteal.x + s_menuSteal.w / 2,
		               s_menuSteal.y + 12, 0.55f, CLR_INK);

		ui_tile(s_menuDelete.x, s_menuDelete.y, s_menuDelete.w, s_menuDelete.h,
		        RGB(255, 92, 116), false);
		ui_text_center("DELETE", s_menuDelete.x + s_menuDelete.w / 2,
		               s_menuDelete.y + 12, 0.55f, CLR_WHITE);

		ui_text_center("B = back inside pages", BOT_W / 2, 226, 0.36f, CLR_DIM);
		break;
	}

	case ST_JOIN_SETUP: {
		layout_join();
		ui_text("JOIN GAME", 14, 10, 0.5f, CLR_SOFT);
		ui_text_right("B = back", BOT_W - 14, 12, 0.38f, CLR_DIM);

		draw_row(s_joinRow[0], "NAME", G.player);
		draw_row(s_joinRow[1], "GAME PIN", G.code[0] ? G.code : "— tap —");
		draw_row(s_joinRow[2], "AVATAR", G.avatar);

		bool ready = G.code[0] && G.player[0] && G.host[0];
		u32 fill = ready ? RGB(56, 214, 160) : RGBA(255, 255, 255, 55);
		ui_tile(s_joinStart.x, s_joinStart.y, s_joinStart.w, s_joinStart.h,
		        fill, false);
		ui_text_center(ready ? "JOIN GAME" : "ENTER A GAME PIN",
		               BOT_W / 2, s_joinStart.y + 15, 0.62f,
		               ready ? CLR_INK : CLR_DIM);
		break;
	}

	case ST_CONNECTING:
		ui_text_center("Talking to the server...", BOT_W / 2, 100, 0.55f, CLR_WHITE);
		ui_text_center("Press B to cancel", BOT_W / 2, 130, 0.42f, CLR_DIM);
		break;

	case ST_LOBBY:
		ui_panel_hi(20, 60, BOT_W - 40, 110, CLR_GLASS);
		ui_text_center("READY", BOT_W / 2, 76, 0.6f, CLR_WHITE);
		ui_text_center(G.player, BOT_W / 2, 106, 0.7f, CLR_GOLD);
		ui_text_center(G.avatar, BOT_W / 2, 138, 0.45f, CLR_SOFT);
		ui_text_center("Press B to leave", BOT_W / 2, 200, 0.42f, CLR_DIM);
		break;

	case ST_QUESTION:
	case ST_WAITING: {
		const bool locked = (G.state == ST_WAITING);

		if (G.qType == QT_MC) {
			layout_answers();
			for (int i = 0; i < G.nAnswers; i++) {
				Rect r = s_tileRect[i];
				bool sel = (G.selected == i);
				u32 fill = ui_answer_color(i);
				if (locked && !sel) fill = ui_fade(fill, 0.35f);

				if (sel) {
					// White halo marks the current pick.
					C2D_DrawRectSolid(r.x - 3, r.y - 3, 0.08f,
					                  r.w + 6, r.h + 6, CLR_WHITE);
				}
				ui_tile(r.x, r.y, r.w, r.h, fill, sel);

				float scale = 0.55f;
				int n = ui_wrap_fit(G.answers[i], r.w - 16, r.h - 10,
				                    0.58f, 0.32f, lines, 3, &scale);
				float lh = ui_line_height(scale);
				float ty = r.y + (r.h - n * lh) / 2;
				for (int k = 0; k < n; k++) {
					ui_text_center(lines[k], r.x + r.w / 2, ty + k * lh, scale, CLR_WHITE);
				}
			}
		} else if (G.qType == QT_SLIDER) {
			layout_slider();
			ui_text("DRAG THE SLIDER", 18, 26, 0.45f, CLR_DIM);

			// Track + knob.
			ui_bar(s_sliderTrack.x, s_sliderTrack.y + s_sliderTrack.h / 2 - 5,
			       s_sliderTrack.w, 10, 1.0f, RGBA(255, 255, 255, 60),
			       RGBA(255, 255, 255, 25));
			float t = (G.sValue - G.sMin) / (G.sMax - G.sMin);
			C2D_DrawCircleSolid(s_sliderTrack.x + t * s_sliderTrack.w,
			                    s_sliderTrack.y + s_sliderTrack.h / 2, 0.5f,
			                    14, locked ? CLR_DIM : CLR_GOLD);

			// Big live value.
			snprintf(buf, sizeof(buf), "%.1f", G.sValue);
			ui_text_center_shadow(buf, BOT_W / 2, 60, 1.0f, CLR_GOLD);
			snprintf(buf, sizeof(buf), "%.0f", G.sMin);
			ui_text(buf, s_sliderTrack.x, 168, 0.42f, CLR_DIM);
			snprintf(buf, sizeof(buf), "%.0f", G.sMax);
			ui_text_right(buf, s_sliderTrack.x + s_sliderTrack.w, 168, 0.42f, CLR_DIM);
			if (!locked) {
				ui_text_center("Drag or use Left/Right for fine steps",
				               BOT_W / 2, 150, 0.38f, CLR_DIM);
			}
		} else if (G.qType == QT_PUZZLE) {
			layout_puzzle();
			ui_text("PUT THEM IN ORDER", 18, 6, 0.42f, CLR_DIM);
			for (int i = 0; i < G.nAnswers; i++) {
				Rect r = s_pzRow[i];
				bool sel = (G.pSel == i);
				u32 fill = sel ? ui_fade(CLR_GOLD, 0.30f) : CLR_GLASS;
				if (locked) fill = ui_fade(fill, 0.5f);
				ui_panel_hi(r.x, r.y, r.w, r.h, fill);
				snprintf(buf, sizeof(buf), "%d.", i + 1);
				ui_text(buf, r.x + 8, r.y + r.h / 2 - 8, 0.4f, CLR_DIM);
				float scale = 0.5f;
				int n = ui_wrap_fit(G.answers[G.pOrder[i]], r.w - 40, r.h - 8,
				                    0.52f, 0.3f, lines, 2, &scale);
				float lh = ui_line_height(scale);
				float ty = r.y + (r.h - n * lh) / 2;
				for (int k = 0; k < n; k++) {
					ui_text(lines[k], r.x + 32, ty + k * lh, scale,
					        sel ? CLR_GOLD : CLR_WHITE);
				}
				// Up / down arrows.
				u32 uf = (i > 0 && !locked) ? RGBA(255, 255, 255, 90) : RGBA(255, 255, 255, 30);
				ui_tile(s_pzUp[i].x, s_pzUp[i].y, s_pzUp[i].w, s_pzUp[i].h, uf, false);
				ui_text_center("^", s_pzUp[i].x + s_pzUp[i].w / 2,
				               s_pzUp[i].y + s_pzUp[i].h / 2 - 9, 0.55f, CLR_WHITE);
				u32 df = (i + 1 < G.nAnswers && !locked) ? RGBA(255, 255, 255, 90) : RGBA(255, 255, 255, 30);
				ui_tile(s_pzDn[i].x, s_pzDn[i].y, s_pzDn[i].w, s_pzDn[i].h, df, false);
				ui_text_center("v", s_pzDn[i].x + s_pzDn[i].w / 2,
				               s_pzDn[i].y + s_pzDn[i].h / 2 - 9, 0.55f, CLR_WHITE);
			}
		} else {
			layout_type();
			ui_text("YOUR ANSWER", 18, 24, 0.45f, CLR_DIM);
			ui_panel_hi(s_typeBox.x, s_typeBox.y, s_typeBox.w, s_typeBox.h,
			            locked ? RGBA(255, 255, 255, 30) : CLR_GLASS);

			if (G.typed[0]) {
				float scale = 0.65f;
				int n = ui_wrap_fit(G.typed, s_typeBox.w - 20, s_typeBox.h - 16,
				                    0.7f, 0.35f, lines, 4, &scale);
				float lh = ui_line_height(scale);
				float ty = s_typeBox.y + (s_typeBox.h - n * lh) / 2;
				for (int k = 0; k < n; k++) {
					ui_text_center(lines[k], BOT_W / 2, ty + k * lh, scale, CLR_WHITE);
				}
			} else {
				ui_text_center("tap to type", BOT_W / 2,
				               s_typeBox.y + s_typeBox.h / 2 - 10, 0.55f, CLR_DIM);
			}
			if (!locked) {
				ui_text_center("Tap the box or press A to open the keyboard",
				               BOT_W / 2, 150, 0.38f, CLR_DIM);
			}
		}

		bool can = true;
		if (locked) can = false;
		else if (G.qType == QT_MC) can = G.selected >= 0;
		else if (G.qType == QT_TYPE) can = G.typed[0] != '\0';

		u32 fill = locked ? RGBA(255, 255, 255, 40)
		                  : (can ? RGB(56, 214, 160) : RGBA(255, 255, 255, 55));
		ui_tile(s_submitRect.x, s_submitRect.y, s_submitRect.w, s_submitRect.h,
		        fill, false);
		const char* label = locked ? "WAITING FOR OTHERS"
		                           : (can ? "SUBMIT" : (G.qType == QT_MC ? "PICK AN ANSWER"
		                                                                 : "TYPE AN ANSWER"));
		ui_text_center(label, BOT_W / 2, s_submitRect.y + 10, 0.58f,
		               can && !locked ? CLR_INK : CLR_DIM);
		break;
	}

	case ST_REVEALED: {
		ui_text_center("YOU ANSWERED", BOT_W / 2, 30, 0.45f, CLR_DIM);
		u32 accent = G.revealCorrect ? CLR_GOOD : CLR_BAD;
		ui_panel_hi(16, 52, BOT_W - 32, 76, ui_fade(accent, 0.35f));

		const char* mine = G.submittedText[0] ? G.submittedText : "(no answer)";
		float scale = 0.6f;
		int n = ui_wrap_fit(mine, BOT_W - 52, 62, 0.65f, 0.34f, lines, 3, &scale);
		float lh = ui_line_height(scale);
		float ty = 52 + (76 - n * lh) / 2;
		for (int k = 0; k < n; k++) {
			ui_text_center(lines[k], BOT_W / 2, ty + k * lh, scale, CLR_WHITE);
		}

		snprintf(buf, sizeof(buf), "Total: %d points", G.myScore);
		ui_text_center(buf, BOT_W / 2, 148, 0.55f, CLR_GOLD);
		ui_text_center("Waiting for the next question...", BOT_W / 2, 196, 0.42f, CLR_DIM);
		break;
	}

	case ST_LEADERBOARD:
	case ST_FINAL: {
		ui_text("STANDINGS", 14, 8, 0.5f, CLR_SOFT);
		if (G.nBoard > 6) ui_text_right("Up/Down", BOT_W - 14, 10, 0.38f, CLR_DIM);

		const float rowH = 30.0f;
		int visible = 6;
		for (int v = 0; v < visible; v++) {
			int i = G.boardScroll + v;
			if (i >= G.nBoard) break;
			float y = 30 + v * (rowH + 3);
			bool me = !strcmp(G.board[i].name, G.player);

			u32 fill = me ? RGBA(255, 255, 255, 105) : CLR_GLASS;
			ui_panel_hi(10, y, BOT_W - 20, rowH, fill);

			snprintf(buf, sizeof(buf), "%d", i + 1);
			ui_text(buf, 18, y + 6, 0.5f, i == 0 ? CLR_GOLD : CLR_SOFT);

			u32 nameClr = G.board[i].left ? CLR_DIM : CLR_WHITE;
			snprintf(buf, sizeof(buf), "%s%s", G.board[i].name,
			         G.board[i].left ? " (left)" : "");
			ui_text(buf, 44, y + 6, 0.48f, nameClr);

			if (G.board[i].lastCorrect >= 0) {
				C2D_DrawCircleSolid(BOT_W - 92, y + rowH / 2, 0.5f, 4,
				                    G.board[i].lastCorrect ? CLR_GOOD : CLR_BAD);
			}
			snprintf(buf, sizeof(buf), "%d", G.board[i].score);
			ui_text_right(buf, BOT_W - 18, y + 6, 0.5f, me ? CLR_GOLD : CLR_WHITE);
		}

		if (G.nBoard == 0) {
			ui_text_center("No scores yet", BOT_W / 2, 100, 0.5f, CLR_DIM);
		}
		ui_text_center(G.state == ST_FINAL ? "Press B to return to setup"
		                                   : "Next question coming up...",
		               BOT_W / 2, 216, 0.42f, CLR_DIM);
		break;
	}

	case ST_CONTROL:
	case ST_SOLO:
	case ST_HOST:
	case ST_STEAL:
	case ST_DELETE:
		break;

	case ST_ERROR: {
		if (G.errorRetry) {
			ui_tile(s_errRetry.x, s_errRetry.y, s_errRetry.w, s_errRetry.h,
			        RGB(56, 214, 160), false);
			ui_text_center("RETRY", s_errRetry.x + s_errRetry.w / 2,
			               s_errRetry.y + 13, 0.6f, CLR_INK);
		}

		ui_tile(s_errBack.x, s_errBack.y, s_errBack.w, s_errBack.h,
		        RGBA(255, 255, 255, 70), false);
		ui_text_center("BACK", s_errBack.x + s_errBack.w / 2,
		               s_errBack.y + 13, 0.6f, CLR_WHITE);
		break;
	}
	}
}

// ---------------------------------------------------------------- user input

static void edit_server(void)
{
	char in[128];
	char cur[128];
	snprintf(cur, sizeof(cur), "%s", G.host);
	if (!keyboard("Server address", cur, in, sizeof(in),
	              SWKBD_TYPE_NORMAL, 100)) {
		return;
	}
	// A colon still works ("host:port"), but the port is asked for separately
	// on the numeric keyboard so any port (80, 8080, ...) is easy to enter.
	char* colon = strrchr(in, ':');
	if (colon) {
		*colon = '\0';
		int p = atoi(colon + 1);
		if (p > 0 && p <= 65535) G.port = p;
	} else {
		char portStr[8];
		char portIn[16];
		snprintf(portStr, sizeof(portStr), "%d", G.port);
		if (keyboard("Port (1-65535)", portStr, portIn, sizeof(portIn),
		             SWKBD_TYPE_NUMPAD, 5)) {
			int p = atoi(portIn);
			if (p > 0 && p <= 65535) G.port = p;
		}
	}
	if (in[0]) snprintf(G.host, sizeof(G.host), "%s", in);
	config_save();
}

static void pick_avatar(void)
{
	// Pull the real list once, the first time the field is touched, so the
	// names line up with the PNGs the host screen looks for.
	static bool fetched = false;
	if (!fetched) {
		fetched = true;
		avatars_fetch();
	}
	if (s_nAvatars == 0) avatars_defaults();
	// Cycle to the next avatar; the list is short and this avoids a
	// second full-screen picker.
	int cur = 0;
	for (int i = 0; i < s_nAvatars; i++) {
		if (!strcmp(s_avatars[i], G.avatar)) { cur = i; break; }
	}
	cur = (cur + 1) % s_nAvatars;
	snprintf(G.avatar, sizeof(G.avatar), "%s", s_avatars[cur]);
	toast("Avatar: %s", G.avatar);
	config_save();
}

// Hit-test rectangles are produced by the draw pass, but input runs first.
// Recomputing them here keeps taps accurate on the frame a state changes.
static void layout_for_state(void)
{
	switch (G.state) {
	case ST_MENU:
		layout_menu();
		break;
	case ST_QUESTION:
	case ST_WAITING:
		if (G.qType == QT_MC) layout_answers();
		else if (G.qType == QT_SLIDER) layout_slider();
		else if (G.qType == QT_PUZZLE) layout_puzzle();
		else layout_type();
		break;
	case ST_ERROR:
		if (G.errorRetry) {
			s_errRetry.x = 16;  s_errRetry.y = 90; s_errRetry.w = 136; s_errRetry.h = 44;
			s_errBack.x  = 168; s_errBack.y  = 90; s_errBack.w  = 136; s_errBack.h  = 44;
		} else {
			s_errBack.x = 60; s_errBack.y = 90; s_errBack.w = 200; s_errBack.h = 44;
		}
		break;
	default:
		break;
	}
}

static void handle_input(u32 kDown, touchPosition* touch, bool touched)
{
	int tx = touch->px, ty = touch->py;

	layout_for_state();

	switch (G.state) {
	case ST_MENU:
		if (G.offline) {
			// Dev bypass: X+Y+SELECT forces online without the check.
			// Armed on SELECT release so key auto-repeat can't re-trigger.
			static bool comboArm = true;
			if (hidKeysUp() & KEY_SELECT) comboArm = true;
			if (comboArm && (hidKeysHeld() & KEY_X) && (hidKeysHeld() & KEY_Y) &&
			    (kDown & KEY_SELECT)) {
				comboArm = false;
				G.offline = false;
				img_set_offline(false);
				solo_offline_mode(false);
				toast("Forced online");
				break;
			}
			// Offline menu: only SOLO (cached quizzes) and DELETE (cached).
			if (touched) {
				if (ui_hit(20, 60, BOT_W - 40, 60, tx, ty)) {
					solo_start(G.host, G.port);
					G.state = ST_SOLO;
				} else if (ui_hit(20, 130, BOT_W - 40, 60, tx, ty)) {
					cachedel_start();
					G.state = ST_CACHEDEL;
				}
			}
			if (kDown & KEY_A) {
				solo_start(G.host, G.port);
				G.state = ST_SOLO;
			}
			if (kDown & KEY_Y) {
				// Check connectivity again before going back online.
				G.pingToOnline = true;
				net_ping_start();
			}
			break;
		}
		if (kDown & KEY_Y) {
			// Drop into offline mode (no check needed going this way).
			G.offline = true;
			img_set_offline(true);
			solo_offline_mode(true);
		}
		if (touched) {
#ifndef PROD_BUILD
			if (ui_hit(s_menuServer.x, s_menuServer.y, s_menuServer.w, s_menuServer.h, tx, ty)) {
				edit_server();
			} else
#endif
			if (ui_hit(s_menuBtn[0].x, s_menuBtn[0].y, s_menuBtn[0].w, s_menuBtn[0].h, tx, ty)) {
				G.state = ST_JOIN_SETUP;
			} else if (ui_hit(s_menuBtn[1].x, s_menuBtn[1].y, s_menuBtn[1].w, s_menuBtn[1].h, tx, ty)) {
				host_start(G.host, G.port);
				G.state = ST_HOST;
			} else if (ui_hit(s_menuBtn[2].x, s_menuBtn[2].y, s_menuBtn[2].w, s_menuBtn[2].h, tx, ty)) {
				char cc[8];
				if (keyboard("Host's control code", "", cc, sizeof(cc),
				             SWKBD_TYPE_NUMPAD, 4)) {
					char err[160];
					if (ctrl_start(G.host, G.port, cc, err, sizeof(err))) {
						G.state = ST_CONTROL;
					} else {
						toast("%s", err);
					}
				}
			} else if (ui_hit(s_menuBtn[3].x, s_menuBtn[3].y, s_menuBtn[3].w, s_menuBtn[3].h, tx, ty)) {
				solo_start(G.host, G.port);
				G.state = ST_SOLO;
			} else if (ui_hit(s_menuSteal.x, s_menuSteal.y, s_menuSteal.w, s_menuSteal.h, tx, ty)) {
				steal_start(G.host, G.port);
				G.state = ST_STEAL;
			} else if (ui_hit(s_menuDelete.x, s_menuDelete.y, s_menuDelete.w, s_menuDelete.h, tx, ty)) {
				delete_start(G.host, G.port);
				G.state = ST_DELETE;
			}
		}
		break;

	case ST_JOIN_SETUP:
		if (touched) {
			if (ui_hit(s_joinRow[0].x, s_joinRow[0].y, s_joinRow[0].w, s_joinRow[0].h, tx, ty)) {
				char in[64];
				if (keyboard("Your name", G.player, in, sizeof(in), SWKBD_TYPE_NORMAL, 20)) {
					snprintf(G.player, sizeof(G.player), "%s", in);
					config_save();
				}
			} else if (ui_hit(s_joinRow[1].x, s_joinRow[1].y, s_joinRow[1].w, s_joinRow[1].h, tx, ty)) {
				char in[16];
				if (keyboard("4-digit game PIN", G.code, in, sizeof(in), SWKBD_TYPE_NUMPAD, 4)) {
					snprintf(G.code, sizeof(G.code), "%s", in);
					config_save();
				}
			} else if (ui_hit(s_joinRow[2].x, s_joinRow[2].y, s_joinRow[2].w, s_joinRow[2].h, tx, ty)) {
				pick_avatar();
			} else if (ui_hit(s_joinStart.x, s_joinStart.y, s_joinStart.w, s_joinStart.h, tx, ty)) {
				if (G.code[0] && G.player[0] && G.host[0]) {
					G.qNumber = 0;
					connect_start();
				} else {
					toast("Enter a game PIN first");
				}
			}
		}
		if (kDown & KEY_A) {
			if (G.code[0] && G.player[0] && G.host[0]) {
				G.qNumber = 0;
				connect_start();
			}
		}
		if (kDown & KEY_B) G.state = ST_MENU;
		break;

	case ST_CONNECTING:
		if (kDown & KEY_B) {
			net_teardown();
			G.state = ST_MENU;
		}
		break;

	case ST_LOBBY:
		if (kDown & KEY_B) {
			net_teardown();
			G.state = ST_MENU;
		}
		break;

	case ST_QUESTION:
		if (G.qType == QT_MC) {
			if (touched) {
				for (int i = 0; i < G.nAnswers; i++) {
					if (ui_hit(s_tileRect[i].x, s_tileRect[i].y,
					           s_tileRect[i].w, s_tileRect[i].h, tx, ty)) {
						G.selected = i;
						break;
					}
				}
			}
			if (kDown & (KEY_DOWN | KEY_RIGHT)) {
				G.selected = (G.selected + 1) % (G.nAnswers > 0 ? G.nAnswers : 1);
			}
			if (kDown & (KEY_UP | KEY_LEFT)) {
				G.selected = (G.selected <= 0 ? G.nAnswers : G.selected) - 1;
			}
		} else if (G.qType == QT_SLIDER) {
			// Drag on the track sets the value; D-pad does fine steps.
			if (touched && ui_hit(s_sliderTrack.x, s_sliderTrack.y,
			                      s_sliderTrack.w, s_sliderTrack.h, tx, ty)) {
				float t = (float)(tx - s_sliderTrack.x) / s_sliderTrack.w;
				if (t < 0.0f) t = 0.0f;
				if (t > 1.0f) t = 1.0f;
				G.sValue = G.sMin + t * (G.sMax - G.sMin);
			}
			float step = (G.sMax - G.sMin) / 100.0f;
			if (step < 0.01f) step = 0.01f;
			if (kDown & KEY_RIGHT) G.sValue += step;
			if (kDown & KEY_LEFT)  G.sValue -= step;
			if (G.sValue < G.sMin) G.sValue = G.sMin;
			if (G.sValue > G.sMax) G.sValue = G.sMax;
		} else if (G.qType == QT_PUZZLE) {
			// Tap an arrow to move a row up/down; D-pad selects and moves.
			if (touched) {
				for (int i = 0; i < G.nAnswers; i++) {
					if (ui_hit(s_pzUp[i].x, s_pzUp[i].y, s_pzUp[i].w, s_pzUp[i].h, tx, ty) && i > 0) {
						int tmp = G.pOrder[i]; G.pOrder[i] = G.pOrder[i - 1]; G.pOrder[i - 1] = tmp;
						G.pSel = i - 1;
						break;
					}
					if (ui_hit(s_pzDn[i].x, s_pzDn[i].y, s_pzDn[i].w, s_pzDn[i].h, tx, ty) && i + 1 < G.nAnswers) {
						int tmp = G.pOrder[i]; G.pOrder[i] = G.pOrder[i + 1]; G.pOrder[i + 1] = tmp;
						G.pSel = i + 1;
						break;
					}
					if (ui_hit(s_pzRow[i].x, s_pzRow[i].y, s_pzRow[i].w, s_pzRow[i].h, tx, ty)) {
						G.pSel = i;
						break;
					}
				}
			}
			if (kDown & KEY_DOWN && G.pSel + 1 < G.nAnswers) G.pSel++;
			if (kDown & KEY_UP && G.pSel > 0) G.pSel--;
			if (kDown & KEY_RIGHT && G.pSel + 1 < G.nAnswers) {
				int i = G.pSel;
				int tmp = G.pOrder[i]; G.pOrder[i] = G.pOrder[i + 1]; G.pOrder[i + 1] = tmp;
				G.pSel++;
			}
			if (kDown & KEY_LEFT && G.pSel > 0) {
				int i = G.pSel;
				int tmp = G.pOrder[i]; G.pOrder[i] = G.pOrder[i - 1]; G.pOrder[i - 1] = tmp;
				G.pSel--;
			}
		} else {
			bool openKbd = (kDown & KEY_A) != 0;
			if (touched && ui_hit(s_typeBox.x, s_typeBox.y,
			                      s_typeBox.w, s_typeBox.h, tx, ty)) {
				openKbd = true;
			}
			if (openKbd) {
				char in[ANS_LEN];
				if (keyboard("Type your answer", G.typed, in, sizeof(in),
				             SWKBD_TYPE_NORMAL, ANS_LEN - 1)) {
					snprintf(G.typed, sizeof(G.typed), "%s", in);
				}
			}
		}

		if (touched && ui_hit(s_submitRect.x, s_submitRect.y,
		                      s_submitRect.w, s_submitRect.h, tx, ty)) {
			submit_answer();
		}
		if ((kDown & KEY_X) ||
		    ((G.qType == QT_MC || G.qType == QT_SLIDER || G.qType == QT_PUZZLE)
		     && (kDown & KEY_A))) {
			submit_answer();
		}
		if (kDown & KEY_B) {
			net_teardown();
			G.state = ST_MENU;
		}
		break;

	case ST_WAITING:
	case ST_REVEALED:
		if (kDown & KEY_B) {
			net_teardown();
			G.state = ST_MENU;
		}
		break;

	case ST_LEADERBOARD:
	case ST_FINAL:
		if (kDown & KEY_DOWN) {
			if (G.boardScroll + 6 < G.nBoard) G.boardScroll++;
		}
		if (kDown & KEY_UP) {
			if (G.boardScroll > 0) G.boardScroll--;
		}
		if (kDown & KEY_B) {
			net_teardown();
			G.state = ST_MENU;
		}
		break;

	case ST_ERROR:
		if (touched) {
			if (G.errorRetry && ui_hit(s_errRetry.x, s_errRetry.y,
			                           s_errRetry.w, s_errRetry.h, tx, ty)) {
				connect_start();
			} else if (ui_hit(s_errBack.x, s_errBack.y, s_errBack.w, s_errBack.h, tx, ty)) {
				G.state = ST_MENU;
			}
		}
		if ((kDown & KEY_A) && G.errorRetry) connect_start();
		if (kDown & KEY_B) G.state = ST_MENU;
		break;

	case ST_CONTROL:
		ctrl_input(kDown, touch, touched);
		break;

	case ST_SOLO:
		solo_input(kDown, touch, touched);
		break;

	case ST_HOST:
		host_input(kDown, touch, touched);
		break;

	case ST_STEAL:
		steal_input(kDown, touch, touched);
		break;

	case ST_DELETE:
		delete_input(kDown, touch, touched);
		break;

	case ST_CACHEDEL:
		cachedel_input(kDown, touch, touched);
		break;
	}
}

// ----------------------------------------------------------------- entry

// Draws the current state's top screen into the active render target.
// Called once per eye when the 3D slider is up, otherwise once (left eye).
// Depth comes from ui_parallax() per layer (see ui.h), not a viewport shift.
static void draw_top_screen(void)
{
	ui_bg(TOP_W, SCR_H, G.time, 0.0f);
	if (G.state == ST_CONTROL) ctrl_draw_top();
	else if (G.state == ST_SOLO) solo_draw_top();
	else if (G.state == ST_HOST) host_draw_top();
	else if (G.state == ST_STEAL) steal_draw_top();
	else if (G.state == ST_DELETE) delete_draw_top();
	else if (G.state == ST_CACHEDEL) cachedel_draw_top();
	else                        draw_top();
}

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;

	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	ui_init();

	C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	C3D_RenderTarget* topR = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	C3D_RenderTarget* bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	bool stereo = false;

	srand((unsigned)osGetTime());

	memset(&G, 0, sizeof(G));
	G.state    = ST_MENU;
	G.selected = -1;
	config_load();
#ifdef PROD_BUILD
	// Production build: the server is fixed; ignore whatever config.txt says.
	snprintf(G.host, sizeof(G.host), "%s", PROD_HOST);
	G.port = PROD_PORT;
#endif
	avatars_defaults();

	if (!net_init()) {
		set_error("Could not start networking (SOC). Is Wi-Fi on?");
	}

	// Startup connectivity check: if 8.8.8.8 does not answer within 15
	// seconds, the app forces offline mode.
	G.bootPing = true;
	net_ping_start();

	u64 startMs = osGetTime();

	while (aptMainLoop()) {
		hidScanInput();
		u32 kDown = hidKeysDown();
		touchPosition touch;
		hidTouchRead(&touch);
		bool touched = (hidKeysDown() & KEY_TOUCH) != 0;

		G.time = (float)(osGetTime() - startMs) / 1000.0f;

		// Offline-mode connectivity handling, only on the setup screen.
		if (G.state == ST_MENU) {
			net_ping_step();
			int ps = net_ping_state();
			if (G.pingToOnline && (ps == 2 || ps == 3)) {
				// A user-requested re-check finished. Only THIS can take the
				// app back online (the startup ping must never override a
				// manual switch).
				G.pingToOnline = false;
				if (ps == 2) {
					G.offline = false;
					img_set_offline(false);
					solo_offline_mode(false);
					toast("Connection OK - back online");
				} else {
					toast("Still no internet - staying offline");
				}
			}
			if (G.bootPing && (ps == 2 || ps == 3)) {
				G.bootPing = false;
				if (ps == 3) {
					// No internet after 15s: force offline mode.
					G.offline = true;
					img_set_offline(true);
					solo_offline_mode(true);
				}
			}
		}

		if ((kDown & KEY_START) && (G.state == ST_MENU || G.state == ST_ERROR)) break;

		// Input first: the software keyboard must not run inside a frame.
		handle_input(kDown, &touch, touched);

		if (G.state == ST_CONTROL) {
			ctrl_step();
			if (ctrl_wants_exit()) {
				const char* e = ctrl_error();
				ctrl_exit();
				if (e && *e) {
					app_error("%s", e);
				} else if (G.state != ST_ERROR) {
					G.state = ST_MENU;
				}
			}
		} else if (G.state == ST_SOLO) {
			solo_step();
			img_step(); // question images also work in solo play
			if (solo_wants_exit()) {
				solo_exit();
				if (G.state != ST_ERROR) G.state = ST_MENU;
			}
		} else if (G.state == ST_HOST) {
			host_step();
			if (host_wants_control()) {
				char cc[8];
				snprintf(cc, sizeof(cc), "%s", host_control_code());
				host_exit();
				char err[160];
				if (ctrl_start(G.host, G.port, cc, err, sizeof(err))) {
					G.state = ST_CONTROL;
				} else {
					app_error("%s", err);
				}
			} else if (host_wants_exit()) {
				host_exit();
				if (G.state != ST_ERROR) G.state = ST_MENU;
			}
		} else if (G.state == ST_STEAL) {
			steal_step();
			img_step(); // the big cover preview loads on the results page
			if (steal_wants_exit()) {
				steal_exit();
				img_reset();
				if (G.state != ST_ERROR) G.state = ST_MENU;
			}
		} else if (G.state == ST_DELETE) {
			delete_step();
			if (delete_wants_exit()) {
				delete_exit();
				if (G.state != ST_ERROR) G.state = ST_MENU;
			}
		} else if (G.state == ST_CACHEDEL) {
			cachedel_step();
			if (cachedel_wants_exit()) {
				cachedel_exit();
				G.state = ST_MENU;
			}
		} else {
			net_update();
			avatars_step();
			if (G.state == ST_QUESTION || G.state == ST_WAITING) img_step();
		}

		// 3D slider: go stereoscopic while the slider is up. Hysteresis so
		// the mode doesn't flicker when the slider sits at the threshold.
		float slider = osGet3DSliderState();
		bool want3d = slider > 0.4f || (stereo && slider > 0.25f);
		if (want3d != stereo) {
			stereo = want3d;
			gfxSet3D(stereo);
		}
		// Depth scales with the slider: barely-up is shallow, full is deep.
		float strength = (slider - 0.2f) / 0.8f;
		if (strength < 0.0f) strength = 0.0f;
		if (strength > 1.0f) strength = 1.0f;

		ui_begin_frame();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		C2D_TargetClear(top, RGB(120, 60, 200));
		C2D_SceneBegin(top);
		ui_stereo_set(stereo ? -1 : 0, stereo ? strength : 0.0f); // left eye
		draw_top_screen();
		if (stereo) {
			C2D_TargetClear(topR, RGB(120, 60, 200));
			C2D_SceneBegin(topR);
			ui_stereo_set(1, strength); // right eye
			draw_top_screen();
		}
		ui_stereo_set(0, 0.0f); // bottom screen is always 2D

		C2D_TargetClear(bot, RGB(120, 60, 200));
		C2D_SceneBegin(bot);
		ui_bg(BOT_W, SCR_H, G.time, 1.7f);
		if (G.state == ST_CONTROL) ctrl_draw_bottom();
		else if (G.state == ST_SOLO) solo_draw_bottom();
		else if (G.state == ST_HOST) host_draw_bottom();
		else if (G.state == ST_STEAL) steal_draw_bottom();
		else if (G.state == ST_DELETE) delete_draw_bottom();
		else if (G.state == ST_CACHEDEL) cachedel_draw_bottom();
		else                        draw_bottom();

		C3D_FrameEnd(0);
	}

	net_teardown();
	net_exit();
	img_reset();
	thumb_reset();
	ui_exit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
