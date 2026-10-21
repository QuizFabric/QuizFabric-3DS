// Framebuffer graphics for the QuizFabric DS build. See gfx.h.

#include "gfx.h"

#include <stdio.h>
#include <string.h>

#include "font8.h"

#define SCR_W 256
#define SCR_H 192

// DS 15-bit palette -> the 8-bit framebuffer index palette (BGR555 values).
static u16 s_pal[COL_COUNT];

static u8* s_top = NULL;   // VRAM_A bitmap
static u8* s_bot = NULL;   // VRAM_C bitmap
static u8* s_cur = NULL;

static u8 px_index(u16 c)
{
	// Nearest palette entry match (palette is tiny and fixed).
	u8 best = COL_BG;
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
	return best;
}

void gfx_init(void)
{
	videoSetMode(MODE_5_2D);
	videoSetModeSub(MODE_5_2D);

	vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
	vramSetBankC(VRAM_C_SUB_BG_0x06200000);

	s_top = (u8*)BG_BMP_RAM(0);
	s_bot = (u8*)BG_BMP_RAM_SUB(0);

	// Palette (RGB555): dark purple bg, panels, text, accents.
	s_pal[COL_BG]    = RGB15(24, 8, 34);
	s_pal[COL_PANEL] = RGB15(46, 20, 60);
	s_pal[COL_TEXT]  = RGB15(31, 31, 31);
	s_pal[COL_DIM]   = RGB15(18, 12, 24);
	s_pal[COL_GOLD]  = RGB15(31, 26, 11);
	s_pal[COL_PINK]  = RGB15(31, 13, 24);
	s_pal[COL_GOOD]  = RGB15(7, 26, 19);
	s_pal[COL_BAD]   = RGB15(31, 11, 14);
	s_pal[COL_INK]   = RGB15(5, 2, 8);
	s_pal[COL_BAR]   = RGB15(12, 6, 18);

	for (int i = 0; i < COL_COUNT; i++) {
		BG_PALETTE[0x100 + i] = s_pal[i];
	}
	BG_PALETTE[0] = s_pal[COL_BG];
	BG_PALETTE_SUB[0x100] = s_pal[COL_BG];

	// The framebuffer holds palette indices; index 0..255 where the palette
	// slot 0 is COL_BG (sub bg uses its own palette, mirrored via dma copy
	// in gfx_frame_begin).
	gfx_set_target_top();
	gfx_clear(COL_BG);
	gfx_set_target_bottom();
	gfx_clear(COL_BG);
}

void gfx_frame_begin(void)
{
	// Mirror the main palette into the sub palette (indices 0..COL_COUNT).
	for (int i = 0; i < COL_COUNT; i++) {
		BG_PALETTE_SUB[0x100 + i] = s_pal[i];
	}
}

void gfx_frame_end(void)
{
	(void)0; // both framebuffers are live in VRAM
}

void gfx_set_target_top(void) { s_cur = s_top; }
void gfx_set_target_bottom(void) { s_cur = s_bot; }

void gfx_clear(u8 idx)
{
	if (!s_cur) return;
	memset(s_cur, idx, SCR_W * SCR_H);
}

void gfx_rect(int x, int y, int w, int h, u8 idx)
{
	if (!s_cur) return;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > SCR_W) w = SCR_W - x;
	if (y + h > SCR_H) h = SCR_H - y;
	if (w <= 0 || h <= 0) return;
	for (int yy = y; yy < y + h; yy++) {
		memset(s_cur + yy * SCR_W + x, idx, w);
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
	int g = (unsigned char)c - 32;
	if (g < 0 || g >= 95) g = 0;
	for (int gy = 0; gy < 8; gy++) {
		u8 row = font8[g][gy];
		for (int gx = 0; gx < 8; gx++) {
			if (!(row & (0x80 >> gx))) continue;
			gfx_rect(x + gx * scale, y + gy * scale, scale, scale, col);
		}
	}
}

void gfx_text_scale(int x, int y, u8 col, const char* s, int scale)
{
	if (!s || !s_cur) return;
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
			// Greedy wrap: fill each line up to maxw.
			int linew = 0;
			const char* start = p;
			while (*p) {
				int cw = 8 * scale;
				if (linew + cw > maxw) break;
				linew += cw;
				p++;
			}
			if (p == start) p++; // single char wider than the box
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
	if (!s_cur || !px) return;
	for (int yy = 0; yy < h; yy++) {
		if (y + yy < 0 || y + yy >= SCR_H) continue;
		u8* row = s_cur + (y + yy) * SCR_W;
		for (int xx = 0; xx < w; xx++) {
			if (x + xx < 0 || x + xx >= SCR_W) continue;
			row[x + xx] = px_index(px[yy * w + xx]);
		}
	}
}
