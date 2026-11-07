// Framebuffer graphics for the QuizFabric DS build. See gfx.h.
//
// DS hardware split:
//   top screen    = MODE 5, 16-bit bitmap - R5 G5 B5 with red in the LOW bits
//   bottom screen = MODE 6, 8-bit bitmap - palette indexed (the sub engine
//                   does NOT support 16-bit bitmaps)
// The sub palette is a 232-step pink/purple gradient ramp (indices 0..231)
// plus the UI colours at fixed slots (232..). Backgrounds are pre-rendered
// ONCE and blitted back each frame: a frozen picture, no animation.

#include "gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font8.h"

#define SCR_W 256
#define SCR_H 192

#define SUB_RAMP 232            // gradient steps in the sub palette
#define SUB_UI(i) (SUB_RAMP + (i)) // UI colour slots

// libnds' RGB15 macro takes 0..31 inputs and does NOT clamp - passing 0..255
// overflows into the upper bits and mangles every colour (the old brown
// screens). Clamp to 5 bits per channel in DS order: red low, blue high.
#define COL555(r, g, b) \
	((u16)((((r) >> 3) & 31) | (((g) >> 3) & 31) << 5 | (((b) >> 3) & 31) << 10))

// 3DS-palette colours in DS R5 G5 B5 order.
static const u16 s_pal[COL_COUNT] = {
	COL555(120, 60, 200),  // COL_BG    (gradient top)
	COL555(96, 60, 140),   // COL_PANEL (glass over the gradient)
	COL555(255, 255, 255), // COL_TEXT
	COL555(190, 160, 220), // COL_DIM
	COL555(255, 208, 92),  // COL_GOLD
	COL555(255, 110, 199), // COL_PINK
	COL555(58, 214, 160),  // COL_GOOD
	COL555(255, 92, 116),  // COL_BAD
	COL555(46, 16, 70),    // COL_INK
	COL555(46, 16, 70),    // COL_BAR
};

static u16* s_top = NULL;   // 16-bit VRAM_A bitmap
static u8*  s_bot = NULL;   // 8-bit  VRAM_C bitmap
static bool s_onTop = true;

// Static background caches (no malloc - one less thing to fail in DSi mode).
static u16 s_bgTop[SCR_W * SCR_H]; // cached u16 gradient
static u8  s_bgBot[SCR_W * SCR_H]; // cached u8 gradient indices

static u16 lerp15(u16 a, u16 b, int t, int tn)
{
	int ar = (a >> 10) & 31, ag = (a >> 5) & 31, ab = a & 31;
	int br = (b >> 10) & 31, bg = (b >> 5) & 31, bb = b & 31;
	int r = ar + ((br - ar) * t) / tn;
	int g = ag + ((bg - ag) * t) / tn;
	int bl = ab + ((bb - ab) * t) / tn;
	return (u16)((r << 10) | (g << 5) | bl);
}

// Nearest UI colour slot for an arbitrary BGR555 (sub-screen blits).
static u8 sub_index(u16 c)
{
	u8 best = 0;
	int bestd = 1 << 30;
	for (int i = 0; i < COL_COUNT; i++) {
		int dr = (int)((c >> 10) & 31) - (int)((s_pal[i] >> 10) & 31);
		int dg = (int)((c >> 5) & 31) - (int)((s_pal[i] >> 5) & 31);
		int db = (int)(c & 31) - (int)(s_pal[i] & 31);
		int d = dr * dr + dg * dg + db * db;
		if (d < bestd) {
			bestd = d;
			best = (u8)i;
		}
	}
	return (u8)SUB_UI(best);
}

void gfx_init(void)
{
	videoSetMode(MODE_5_2D);   // main: 16-bit bitmap
	videoSetModeSub(MODE_6_2D); // sub:  8-bit bitmap (no 16-bit on sub!)
#ifndef PROD_BUILD
	gfx_dbg(10);
#endif

	vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
	vramSetBankC(VRAM_C_SUB_BG_0x06200000);
#ifndef PROD_BUILD
	gfx_dbg(20);
#endif

	int mainBg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
	int subBg  = bgInitSub(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);

	s_top = bgGetGfxPtr(mainBg);
	s_bot = (u8*)bgGetGfxPtr(subBg);
#ifndef PROD_BUILD
	gfx_dbg(30);
#endif

	if (isDSiMode) {
		// On the DSi, BGCNT bits 14-15 are the Screen Base Block, not the
		// NTR bitmap-size encoding that libnds wrote (bit 14 set = 256x256).
		// With bit 14 set both engines would read VRAM block 1 (0x06200000):
		// the main engine would show the sub engine's pixels as 16-bit
		// (a black top screen). Force SBB 0 (0x06000000) on the main engine
		// and SBB 1 (0x06200000) on the sub engine.
		REG_BG3CNT &= ~(3 << 14);
		REG_BG3CNT_SUB |= (1 << 14);
	}
#ifndef PROD_BUILD
	gfx_dbg(40);
#endif

	u16 topC = COL555(120, 60, 200);
	u16 botC = COL555(40, 8, 60);

	// Sub palette: gradient ramp + UI colours.
	for (int i = 0; i < SUB_RAMP; i++) {
		BG_PALETTE_SUB[i] = lerp15(topC, botC, i, SUB_RAMP - 1);
	}
	for (int i = 0; i < COL_COUNT; i++) {
		BG_PALETTE_SUB[SUB_UI(i)] = s_pal[i];
	}

	// Cached backgrounds (static arrays).
	for (int y = 0; y < SCR_H; y++) {
		u16 c = lerp15(topC, botC, y, SCR_H - 1);
		for (int x = 0; x < SCR_W; x++) s_bgTop[y * SCR_W + x] = c;
	}
	for (int y = 0; y < SCR_H; y++) {
		u8 idx = (u8)(y * (SUB_RAMP - 1) / (SCR_H - 1));
		memset(s_bgBot + y * SCR_W, idx, SCR_W);
	}
#ifndef PROD_BUILD
	gfx_dbg(60);
#endif

	gfx_set_target_top();
	gfx_clear(COL_BG);
#ifndef PROD_BUILD
	gfx_dbg(70);
#endif
	gfx_set_target_bottom();
	gfx_clear(COL_BG);
#ifndef PROD_BUILD
	gfx_dbg(80);
#endif
}

void gfx_frame_begin(void) { (void)0; }
void gfx_frame_end(void) { (void)0; }

void gfx_set_target_top(void) { s_onTop = true; }
void gfx_set_target_bottom(void) { s_onTop = false; }

void gfx_clear(u8 idx)
{
	(void)idx;
	if (s_onTop) {
		for (int i = 0; i < SCR_W * SCR_H; i++) s_top[i] = s_bgTop[i];
	} else {
		for (int i = 0; i < SCR_W * SCR_H; i++) s_bot[i] = s_bgBot[i];
	}
}

void gfx_rect(int x, int y, int w, int h, u8 idx)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > SCR_W) w = SCR_W - x;
	if (y + h > SCR_H) h = SCR_H - y;
	if (w <= 0 || h <= 0) return;
	if (s_onTop) {
		u16 c = s_pal[idx];
		for (int yy = y; yy < y + h; yy++) {
			u16* row = s_top + yy * SCR_W;
			for (int xx = x; xx < x + w; xx++) row[xx] = c;
		}
	} else {
		u8 c = (u8)SUB_UI(idx);
		for (int yy = y; yy < y + h; yy++) {
			memset(s_bot + yy * SCR_W + x, c, w);
		}
	}
}

void gfx_bar(int x, int y, int w, int h, float pct, u8 fill, u8 track)
{
	gfx_rect(x, y, w, h, track);
	if (pct < 0.0f) pct = 0.0f;
	if (pct > 1.0f) pct = 1.0f;
	gfx_rect(x, y, (int)(w * pct), h, fill);
}

static void put_glyph(int x, int y, u8 col, char c, int scale)
{
	int g = (unsigned char)c;
	if (g > 127) g = 0;
	for (int gy = 0; gy < 8; gy++) {
		u8 row = font8[g][gy];
		for (int gx = 0; gx < 8; gx++) {
			if (!(row & (0x80 >> gx))) continue;
			for (int sy = 0; sy < scale; sy++) {
				int yy = y + gy * scale + sy;
				if (yy < 0 || yy >= SCR_H) continue;
				if (s_onTop) {
					u16* lr = s_top + yy * SCR_W;
					for (int sx = 0; sx < scale; sx++) lr[x + gx * scale + sx] = s_pal[col];
				} else {
					u8* lr = s_bot + yy * SCR_W;
					memset(lr + x + gx * scale, SUB_UI(col), scale);
				}
			}
		}
	}
}

void gfx_text_scale(int x, int y, u8 col, const char* s, int scale)
{
	if (!s) return;
	if (scale < 1) scale = 1;
	int cx = x;
	for (const char* p = s; *p; p++) {
		put_glyph(cx, y, col, *p, scale);
		cx += 8 * scale;
	}
}

void gfx_text(int x, int y, u8 col, const char* s)
{
	gfx_text_scale(x, y, col, s, 1);
}

int gfx_text_width(const char* s, int scale)
{
	if (!s) return 0;
	if (scale < 1) scale = 1;
	return (int)strlen(s) * 8 * scale;
}

void gfx_text_center(int cx, int y, u8 col, const char* s, int scale)
{
	gfx_text_scale(cx - gfx_text_width(s, scale) / 2, y, col, s, scale);
}

int gfx_wrap_fit(const char* s, int maxw, int maxh, int scaleHi, int scaleLo,
                 char lines[][96], int maxLines, int* scaleOut)
{
	if (!s || !s[0]) {
		*scaleOut = scaleHi;
		return 0;
	}
	int scale = scaleHi;
	for (;;) {
		int n = 0;
		const char* p = s;
		while (*p && n < maxLines) {
			int linew = 0;
			const char* start = p;
			while (*p) {
				int cw = 8 * scale;
				if (linew + cw > maxw) break;
				linew += cw;
				p++;
			}
			if (p == start) p++;
			int len = (int)(p - start);
			if (len >= 96) len = 95;
			memcpy(lines[n], start, len);
			lines[n][len] = '\0';
			n++;
		}
		int total = n * 8 * scale;
		if (total <= maxh || scale <= scaleLo) {
			*scaleOut = scale;
			return n;
		}
		scale--;
		if (scale < scaleLo) scale = scaleLo;
	}
}

void gfx_blit555(int x, int y, int w, int h, const u16* px)
{
	if (!px) return;
	for (int yy = 0; yy < h; yy++) {
		if (y + yy < 0 || y + yy >= SCR_H) continue;
		if (s_onTop) {
			u16* row = s_top + (y + yy) * SCR_W;
			for (int xx = 0; xx < w; xx++) {
				if (x + xx < 0 || x + xx >= SCR_W) continue;
				row[x + xx] = px[yy * w + xx];
			}
		} else {
			u8* row = s_bot + (y + yy) * SCR_W;
			for (int xx = 0; xx < w; xx++) {
				if (x + xx < 0 || x + xx >= SCR_W) continue;
				row[x + xx] = sub_index(px[yy * w + xx]);
			}
		}
	}
}

// ------------------------------------------------------------------ debug

static void dbg_hold_frames(int n)
{
	for (int f = 0; f < n; f++) {
		while (REG_VCOUNT >= 192);
		while (REG_VCOUNT < 192);
	}
}

static void dbg_digit(u8* fb, int x0, int y0, int digit, int scale, u8 fg, u8 bg)
{
	int g = '0' + (digit % 10);
	for (int gy = 0; gy < 8; gy++) {
		u8 row = font8[g][gy];
		for (int gx = 0; gx < 8; gx++) {
			u8 c = (row & (0x80 >> gx)) ? fg : bg;
			for (int sy = 0; sy < scale; sy++) {
				int yy = y0 + gy * scale + sy;
				if (yy < 0 || yy >= SCR_H) continue;
				u8* lr = fb + yy * SCR_W;
				for (int sx = 0; sx < scale; sx++) {
					int xx = x0 + gx * scale + sx;
					if (xx < 0 || xx >= SCR_W) continue;
					lr[xx] = c;
				}
			}
		}
	}
}

void gfx_dbg(int code)
{
	// Self-contained: configure the sub engine + its own palette colours so
	// the number displays even before gfx_init has run, then hold it for
	// ~1s so it cannot be missed. Bottom screen ONLY - the top screen's
	// VRAM bank is under suspicion in DSi mode and must not be touched here.
	videoSetModeSub(MODE_6_2D);
	vramSetBankC(VRAM_C_SUB_BG_0x06200000);
	bgInitSub(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
	BG_PALETTE_SUB[234] = COL555(255, 255, 255);
	BG_PALETTE_SUB[240] = COL555(0, 0, 0);

	u8* fb = (u8*)BG_GFX_SUB; // direct: works even before bgGetGfxPtr
	u8 bgc = 234;
	u8 fgc = 240;
	for (int y = 0; y < SCR_H; y++) memset(fb + y * SCR_W, bgc, SCR_W);

	int scale = 6, dw = 8 * scale;
	int total = 3 * dw + 2 * scale;
	int x0 = (SCR_W - total) / 2;
	int y0 = (SCR_H - 8 * scale) / 2;
	for (int i = 0; i < 3; i++) {
		int d = (code / (i == 0 ? 100 : i == 1 ? 10 : 1)) % 10;
		dbg_digit(fb, x0 + i * (dw + scale), y0, d, scale, fgc, bgc);
	}

	dbg_hold_frames(60); // ~1 second
}

void gfx_dbg_corner(int seconds)
{
	u8* fb = (u8*)BG_GFX_SUB;
	int scale = 2;
	int x0 = SCR_W - 2 * 8 * scale - 4;
	int y0 = SCR_H - 8 * scale - 4;
	dbg_digit(fb, x0, y0, (seconds / 10) % 10, scale, 240, 234);
	dbg_digit(fb, x0 + 8 * scale, y0, seconds % 10, scale, 240, 234);
}
