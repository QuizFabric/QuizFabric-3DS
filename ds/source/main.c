// QuizFabric DS client - boot skeleton. This is the foundation of the DS
// port: framebuffer graphics, touch keyboard, and the app shell. The
// Socket.IO game logic (ws.c/sio.c ports) plugs in next.
//
// DS constraints by design:
//  - no GIF animation: "Cannot load GIF. DS is not powerful enough to
//    load GIFs. Use a 3DS instead."
//  - question images arrive as tiny raw RGB555 bitmaps (see the server's
//    .ds converter) and are blitted 1:1 - very low resolution to save RAM.

#include <nds.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "kbd.h"

#ifdef PROD_BUILD
#define PROD_HOST "play.lucasgeitgey.com"
#define PROD_PORT 80
#endif

// Bump per change; the setup screen shows this.
#define APP_BUILD "1"

static const char GIF_MSG[] =
    "Cannot load GIF. DS is not powerful enough to load GIFs. Use a 3DS instead.";

static char s_server[64] = "play.lucasgeitgey.com:80";

static void draw_top(void)
{
	gfx_set_target_top();
	gfx_clear(COL_BG);

	gfx_text_center(DS_TOP_W / 2, 24, COL_TEXT, "QUIZFABRIC", 2);
	gfx_text_center(DS_TOP_W / 2, 52, COL_DIM, "D S   C L I E N T", 1);

	gfx_rect(16, 72, DS_TOP_W - 32, 46, COL_PANEL);
	gfx_text_center(DS_TOP_W / 2, 82, COL_TEXT, s_server, 1);
#ifdef PROD_BUILD
	gfx_text_center(DS_TOP_W / 2, 100, COL_DIM, "PRODUCTION - fixed server", 1);
#else
	gfx_text_center(DS_TOP_W / 2, 100, COL_DIM, "server set at build time", 1);
#endif

	char build[48];
#ifdef PROD_BUILD
	snprintf(build, sizeof(build), "PRODUCTION build %s", APP_BUILD);
#else
	snprintf(build, sizeof(build), "build %s", APP_BUILD);
#endif
	gfx_text(4, 184, COL_DIM, build);
}

static void draw_bottom(void)
{
	gfx_set_target_bottom();
	gfx_clear(COL_BG);
	gfx_text(8, 8, COL_TEXT, "SETUP");

	gfx_rect(8, 30, DS_BOT_W - 16, 24, COL_PINK);
	gfx_text_center(DS_BOT_W / 2, 38, COL_INK, "JOIN", 1);

	gfx_rect(8, 58, DS_BOT_W - 16, 24, COL_PANEL);
	gfx_text_center(DS_BOT_W / 2, 66, COL_TEXT, "HOST", 1);

	gfx_rect(8, 86, DS_BOT_W - 16, 24, COL_PANEL);
	gfx_text_center(DS_BOT_W / 2, 94, COL_TEXT, "SOLO", 1);

	gfx_rect(8, 114, DS_BOT_W - 16, 24, COL_PANEL);
	gfx_text_center(DS_BOT_W / 2, 122, COL_TEXT, "KAHOOT", 1);

	gfx_rect(8, 142, DS_BOT_W - 16, 40, COL_PANEL);
	char lines[3][96];
	int scale = 1;
	int n = gfx_wrap_fit(GIF_MSG, DS_BOT_W - 24, 36, 1, 1, lines, 3, &scale);
	for (int i = 0; i < n; i++) {
		gfx_text_center(DS_BOT_W / 2, 146 + i * 10, COL_DIM, lines[i], 1);
	}
}

int main(void)
{
	defaultExceptionHandler();

	powerOn(POWER_ALL);

	irqEnable(IRQ_VBLANK);

	gfx_init();
	kbd_init();

#ifdef PROD_BUILD
	snprintf(s_server, sizeof(s_server), "%s:%d", PROD_HOST, PROD_PORT);
#endif

	while (1) {
		scanKeys();
		u32 down = keysDown();
		u32 held = keysHeld();
		(void)held;

		touchPosition touch;
		touchRead(&touch);
		(void)touch;

		if (down & KEY_START) break;

		draw_top();
		draw_bottom();

		gfx_frame_begin();
		swiWaitForVBlank();
		gfx_frame_end();
	}

	return 0;
}
