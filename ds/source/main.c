// QuizFabric DS client. Setup screen mirrors the 3DS app's touch-screen
// layout (SERVER / NAME / GAME PIN / AVATAR rows + JOIN GAME), and SOLO
// plays quizzes straight from the server. Frozen gradient background, no
// animation.
//
// DS constraints by design:
//  - no GIF animation: "Cannot load GIF. DS is not powerful enough to
//    load GIFs. Use a 3DS instead."
//  - question images arrive as tiny raw RGB555 bitmaps (server .ds format).

#include <nds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "kbd.h"
#include "net.h"
#include "solo.h"

#ifdef PROD_BUILD
#define PROD_HOST "play.lucasgeitgey.com"
#define PROD_PORT 80
#endif

// Bump per change; matches the 3DS build numbering.
#define APP_BUILD "50"

static const char GIF_MSG[] =
    "Cannot load GIF. DS is not powerful enough to load GIFs. Use a 3DS instead.";

typedef enum { ST_SETUP, ST_SOLO } State;

static char s_server[64] = "play.lucasgeitgey.com:80";
static char s_name[32]   = "";
static char s_pin[8]     = "";
static int  s_avatar     = 0;

static const char* s_avatars[4] = { "Pusheen", "Pig", "Steve", "Alex" };

static char s_toast[64] = "";
static int  s_toastFrames = 0;

static int      s_wifiState = 0;
static unsigned s_wifiStartMs = 0;

typedef struct { int x, y, w, h; } Rect;

static Rect s_rowServer, s_rowName, s_rowPin, s_rowAvatar;
static Rect s_btnJoin, s_btnSolo, s_btnKahoot;

static bool hit(const Rect* r, int tx, int ty)
{
	return tx >= r->x && tx < r->x + r->w && ty >= r->y && ty < r->y + r->h;
}

static void toast(const char* msg)
{
	snprintf(s_toast, sizeof(s_toast), "%s", msg);
	s_toastFrames = 120;
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

static void parse_server(char* host, int hostsz, int* port)
{
	char* colon = strrchr(s_server, ':');
	if (colon) {
		snprintf(host, hostsz, "%.*s", (int)(colon - s_server), s_server);
		*port = atoi(colon + 1);
		if (*port <= 0) *port = 80;
	} else {
		snprintf(host, hostsz, "%s", s_server);
		*port = 80;
	}
}

static void draw_top(void)
{
	gfx_set_target_top();
	gfx_clear(COL_BG);

	gfx_text_center(DS_TOP_W / 2, 14, COL_TEXT, "QUIZFABRIC", 2);
	gfx_text_center(DS_TOP_W / 2, 40, COL_DIM, "D S   C L I E N T", 1);

	if (s_pin[0]) {
		char pin[48];
		snprintf(pin, sizeof(pin), "GAME PIN  %s", s_pin);
		gfx_text_center(DS_TOP_W / 2, 66, COL_GOLD, pin, 1);
	} else {
		gfx_text_center(DS_TOP_W / 2, 66, COL_DIM, "NO GAME PIN SET", 1);
	}

	if (s_wifiState == 0) {
		gfx_text_center(DS_TOP_W / 2, 92, COL_DIM, "Checking connection...", 1);
	} else if (s_wifiState == 1) {
		char ip[64];
		snprintf(ip, sizeof(ip), "Wi-Fi OK - %s", net_ip());
		gfx_text_center(DS_TOP_W / 2, 92, COL_GOOD, ip, 1);
	} else {
		gfx_text_center(DS_TOP_W / 2, 92, COL_BAD, "Wi-Fi failed - check settings", 1);
	}

#ifndef PROD_BUILD
	gfx_rect(16, 114, DS_TOP_W - 32, 22, COL_PANEL);
	gfx_text_center(DS_TOP_W / 2, 120, COL_TEXT, s_server, 1);
#endif

	{
		char av[64];
		snprintf(av, sizeof(av), "Avatar: %s", s_avatars[s_avatar & 3]);
		gfx_text_center(DS_TOP_W / 2, 148, COL_DIM, av, 1);
	}

	{
		char lines[3][96];
		int scale = 1;
		int n = gfx_wrap_fit(GIF_MSG, DS_TOP_W - 32, 26, 1, 1, lines, 3, &scale);
		for (int i = 0; i < n && i < 2; i++) {
			gfx_text_center(DS_TOP_W / 2, 162 + i * 10, COL_DIM, lines[i], 1);
		}
	}

	if (s_toastFrames > 0) {
		gfx_rect(DS_TOP_W / 2 - gfx_text_width(s_toast, 1) / 2 - 8, 2, 16, 14,
		         COL_BAD);
		gfx_text_center(DS_TOP_W / 2, 4, COL_TEXT, s_toast, 1);
	}

	char build[48];
#ifdef PROD_BUILD
	snprintf(build, sizeof(build), "PRODUCTION build %s", APP_BUILD);
#else
	snprintf(build, sizeof(build), "build %s", APP_BUILD);
#endif
	gfx_text(4, 182, COL_DIM, build);
}

static void draw_row(const Rect* r, const char* label, const char* value,
                     bool pressed)
{
	gfx_rect(r->x, r->y, r->w, r->h, pressed ? COL_DIM : COL_PANEL);
	gfx_text(r->x + 8, r->y + 6, COL_DIM, label);
	int vw = gfx_text_width(value, 1);
	gfx_text(r->x + r->w - 8 - vw, r->y + 6, COL_TEXT, value);
}

static void draw_btn(const Rect* r, const char* label, u8 col, bool pressed)
{
	gfx_rect(r->x, r->y, r->w, r->h, pressed ? COL_DIM : col);
	gfx_text_center(r->x + r->w / 2, r->y + r->h / 2 - 4, COL_TEXT, label, 1);
}

static void draw_bottom(bool pServer, bool pName, bool pPin, bool pAvatar,
                        bool pJoin, bool pSolo, bool pKahoot)
{
	gfx_set_target_bottom();
	gfx_clear(COL_BG);
	gfx_text(8, 4, COL_TEXT, "SETUP");

	s_rowServer.x = 8; s_rowServer.y = 24; s_rowServer.w = DS_BOT_W - 16; s_rowServer.h = 20;
	s_rowName.x   = 8; s_rowName.y   = 46; s_rowName.w   = DS_BOT_W - 16; s_rowName.h   = 20;
	s_rowPin.x    = 8; s_rowPin.y    = 68; s_rowPin.w    = DS_BOT_W - 16; s_rowPin.h    = 20;
	s_rowAvatar.x = 8; s_rowAvatar.y = 90; s_rowAvatar.w = DS_BOT_W - 16; s_rowAvatar.h = 20;

	s_btnJoin.x   = 8; s_btnJoin.y   = 116; s_btnJoin.w   = DS_BOT_W - 16; s_btnJoin.h   = 30;
	s_btnSolo.x   = 8; s_btnSolo.y   = 150; s_btnSolo.w   = 116; s_btnSolo.h   = 24;
	s_btnKahoot.x = 132; s_btnKahoot.y = 150; s_btnKahoot.w = 116; s_btnKahoot.h = 24;

#ifdef PROD_BUILD
	(void)pServer;
#else
	draw_row(&s_rowServer, "SERVER", s_server, pServer);
#endif
	draw_row(&s_rowName, "NAME", s_name[0] ? s_name : "— tap —", pName);
	draw_row(&s_rowPin, "GAME PIN", s_pin[0] ? s_pin : "— tap —", pPin);
	draw_row(&s_rowAvatar, "AVATAR", s_avatars[s_avatar & 3], pAvatar);

	draw_btn(&s_btnJoin, "JOIN GAME", COL_GOOD, pJoin);
	draw_btn(&s_btnSolo, "SOLO", COL_GOLD, pSolo);
	draw_btn(&s_btnKahoot, "KAHOOT", COL_PINK, pKahoot);

	gfx_text_center(DS_BOT_W / 2, 182, COL_DIM, "START = quit", 1);
}

#ifdef MINIMAL
// Absolute-minimum test: does the DSi even reach main()? Shows a big "5"
// in a white box on the bottom screen and holds it until power-off.
// Built with: make MINIMAL=1 && make MINIMAL=1 dsiware
int main(void)
{
	defaultExceptionHandler();
	gfx_dbg(5);
	for (;;) {
		while (REG_VCOUNT >= 192);
		while (REG_VCOUNT < 192);
	}
	return 0;
}
#else
int main(void)
{
	defaultExceptionHandler();

#ifndef PROD_BUILD
	gfx_dbg(5); // reached main at all?
#endif

	powerOn(POWER_ALL);

	gfx_init();
#ifndef PROD_BUILD
	gfx_dbg(91);
#endif
	kbd_init();
#ifndef PROD_BUILD
	gfx_dbg(92);
#endif
	net_init();
	s_wifiStartMs = net_ticks_ms();
#ifndef PROD_BUILD
	gfx_dbg(93);
#endif

#ifdef PROD_BUILD
	snprintf(s_server, sizeof(s_server), "%s:%d", PROD_HOST, PROD_PORT);
#endif

	State state = ST_SETUP;

	while (1) {
		scanKeys();
		u32 down = keysDown();

		touchPosition touch;
		touchRead(&touch);
		bool tapped = (down & KEY_TOUCH) != 0;

		if (down & KEY_START) break;

		// Wi-Fi status.
		if (s_wifiState == 0) {
			s_wifiState = net_wifi_state();
			if (s_wifiState == 0 && net_ticks_ms() - s_wifiStartMs > 15000u) {
				s_wifiState = -1; // gave up
			}
		}

		if (state == ST_SOLO) {
			solo_step();
			if (solo_wants_exit()) {
				solo_exit();
				state = ST_SETUP;
			}
		} else {
			bool pServer = false, pName = false, pPin = false, pAvatar = false;
			bool pJoin = false, pSolo = false, pKahoot = false;
			if (tapped) {
				if (hit(&s_rowServer, touch.px, touch.py)) {
					pServer = true;
#ifndef PROD_BUILD
					ask_text("Server address", s_server, sizeof(s_server));
#endif
				} else if (hit(&s_rowName, touch.px, touch.py)) {
					pName = true;
					ask_text("Your name", s_name, sizeof(s_name));
				} else if (hit(&s_rowPin, touch.px, touch.py)) {
					pPin = true;
					ask_text("4-digit game PIN", s_pin, sizeof(s_pin));
				} else if (hit(&s_rowAvatar, touch.px, touch.py)) {
					pAvatar = true;
					s_avatar = (s_avatar + 1) & 3;
				} else if (hit(&s_btnJoin, touch.px, touch.py)) {
					pJoin = true;
					toast("JOIN: coming in the full port");
				} else if (hit(&s_btnSolo, touch.px, touch.py)) {
					pSolo = true;
					char host[64];
					int port;
					parse_server(host, sizeof(host), &port);
					solo_enter(host, port);
					state = ST_SOLO;
				} else if (hit(&s_btnKahoot, touch.px, touch.py)) {
					pKahoot = true;
					toast("KAHOOT: coming in the full port");
				}
			}

			if (s_toastFrames > 0) s_toastFrames--;

			draw_top();
			draw_bottom(pServer, pName, pPin, pAvatar, pJoin, pSolo, pKahoot);
		}

		if (state == ST_SOLO) {
			solo_draw_top();
			solo_draw_bottom();
		}

#ifndef PROD_BUILD
		{
			// Tiny seconds counter in the corner: if this stops climbing,
			// the app froze right after boot.
			static unsigned bootMs = 0;
			if (!bootMs) bootMs = net_ticks_ms();
			gfx_dbg_corner((int)((net_ticks_ms() - bootMs) / 1000u) % 60);
		}
#endif

		net_wait_vblank();
	}

	net_disconnect();
	return 0;
}
#endif // MINIMAL
