#include "ui.h"

#include "emoji_bin.h"
#include "emoji_map.h"

#include <math.h>
#include <string.h>

#define TEXTBUF_GLYPHS 8192

static C2D_TextBuf s_frameBuf   = NULL; // reset every frame
static C2D_TextBuf s_measureBuf = NULL; // scratch, cleared after each measure

// ------------------------------------------------------------------ emoji
// A Twemoji atlas (data/emoji.bin, tiled RGBA8) embedded in the binary;
// text functions render matching UTF-8 codepoints as sprites.

static C3D_Tex    s_emojiTex;
static C2D_Image  s_emojiImg[EMOJI_COUNT];
static Tex3DS_SubTexture s_emojiSub[EMOJI_COUNT];
static bool       s_emojiOk = false;

static void emoji_init(void)
{
	// Format: u16 w, u16 h, then w*h*4 tiled RGBA8 bytes (ABGR texel
	// order). tools/make_emoji_atlas.py builds the atlas with the same
	// converter the server runs for question images, so it is already
	// swizzled into the PICA200's native tiled layout. C3D_TexUpload is a
	// plain memcpy - the bytes must go up exactly as they are.
	u16 w = (u16)(emoji_bin[0] | (emoji_bin[1] << 8));
	u16 h = (u16)(emoji_bin[2] | (emoji_bin[3] << 8));
	if (w < 8 || h < 8 || emoji_bin_size != 4u + (u32)w * h * 4) return;

	if (!C3D_TexInit(&s_emojiTex, w, h, GPU_RGBA8)) return;
	C3D_TexSetFilter(&s_emojiTex, GPU_LINEAR, GPU_NEAREST);
	C3D_TexSetWrap(&s_emojiTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	C3D_TexUpload(&s_emojiTex, emoji_bin + 4);

	float tw = (float)w, th = (float)h;
	for (int i = 0; i < EMOJI_COUNT; i++) {
		int gx = (i % EMOJI_COLS) * EMOJI_TILE;
		int gy = (i / EMOJI_COLS) * EMOJI_TILE;
		s_emojiSub[i] = (Tex3DS_SubTexture){
			(u16)EMOJI_TILE, (u16)EMOJI_TILE,
			(float)gx / tw, 1.0f - (float)gy / th,
			(float)(gx + EMOJI_TILE) / tw, 1.0f - (float)(gy + EMOJI_TILE) / th };
		s_emojiImg[i] = (C2D_Image){ &s_emojiTex, &s_emojiSub[i] };
	}
	s_emojiOk = true;
}

// Decodes the UTF-8 codepoint at s. Returns its atlas tile, -1 for anything
// that is not a known emoji (the font renders its fallback), or -2 for a
// variation selector (drawn as nothing, zero advance). *lenOut is the byte
// length consumed.
static int emoji_tile(const char* s, int* lenOut)
{
	const unsigned char* p = (const unsigned char*)s;
	unsigned cp;
	if (*p < 0x80) {
		*lenOut = 1;
		return -1;
	}
	if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
	    (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
		cp = ((unsigned)(p[0] & 0x07) << 18) |
		     ((unsigned)(p[1] & 0x3F) << 12) |
		     ((unsigned)(p[2] & 0x3F) << 6)  |
		     (unsigned)(p[3] & 0x3F);
		*lenOut = 4;
	} else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
	           (p[2] & 0xC0) == 0x80) {
		cp = ((unsigned)(p[0] & 0x0F) << 12) |
		     ((unsigned)(p[1] & 0x3F) << 6)  |
		     (unsigned)(p[2] & 0x3F);
		*lenOut = 3;
	} else {
		*lenOut = 1;
		return -1;
	}
	if (cp == 0xFE0F || cp == 0xFE0E) return -2; // emoji variation selector
	for (int i = 0; i < EMOJI_COUNT; i++) {
		if (emoji_map[i].cp == cp) return (int)emoji_map[i].idx;
	}
	return -1; // unknown; the font renders its fallback
}

#define BUBBLE_COUNT 14

typedef struct {
	float x, y, r, speed, drift, alpha;
} Bubble;

static Bubble s_bubbles[BUBBLE_COUNT];

void ui_init(void)
{
	s_frameBuf   = C2D_TextBufNew(TEXTBUF_GLYPHS);
	s_measureBuf = C2D_TextBufNew(1024);

	emoji_init();

	// Deterministic scatter: a small LCG keeps the layout stable across runs
	// without pulling in rand(), which the network code reseeds.
	unsigned seed = 0x9E3779B9u;
	for (int i = 0; i < BUBBLE_COUNT; i++) {
		seed = seed * 1103515245u + 12345u;
		float a = (float)((seed >> 16) & 0x7FFF) / 32767.0f;
		seed = seed * 1103515245u + 12345u;
		float b = (float)((seed >> 16) & 0x7FFF) / 32767.0f;
		seed = seed * 1103515245u + 12345u;
		float c = (float)((seed >> 16) & 0x7FFF) / 32767.0f;

		s_bubbles[i].x     = a * 400.0f;
		s_bubbles[i].y     = b * 240.0f;
		s_bubbles[i].r     = 6.0f + c * 26.0f;
		s_bubbles[i].speed = 4.0f + a * 11.0f;
		s_bubbles[i].drift = 6.0f + b * 14.0f;
		s_bubbles[i].alpha = 0.05f + c * 0.07f;
	}
}

void ui_exit(void)
{
	if (s_frameBuf)   C2D_TextBufDelete(s_frameBuf);
	if (s_measureBuf) C2D_TextBufDelete(s_measureBuf);
	s_frameBuf = s_measureBuf = NULL;
	if (s_emojiOk) {
		C3D_TexDelete(&s_emojiTex);
		s_emojiOk = false;
	}
}

void ui_begin_frame(void)
{
	C2D_TextBufClear(s_frameBuf);
}

// ------------------------------------------------------------------- colours

u32 ui_lerp(u32 a, u32 b, float t)
{
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	int ar =  a        & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF, aa = (a >> 24) & 0xFF;
	int br =  b        & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF, ba = (b >> 24) & 0xFF;
	int r = ar + (int)((br - ar) * t);
	int g = ag + (int)((bg - ag) * t);
	int bl = ab + (int)((bb - ab) * t);
	int al = aa + (int)((ba - aa) * t);
	return C2D_Color32((u8)r, (u8)g, (u8)bl, (u8)al);
}

u32 ui_fade(u32 c, float a)
{
	if (a < 0.0f) a = 0.0f;
	if (a > 1.0f) a = 1.0f;
	u32 alpha = (u32)(((c >> 24) & 0xFF) * a);
	return (c & 0x00FFFFFF) | (alpha << 24);
}

u32 ui_answer_color(int i)
{
	switch (i & 7) {
	case 0: return RGB(255,  79, 129); // rose
	case 1: return RGB(124,  77, 255); // violet
	case 2: return RGB(255, 159,  69); // amber
	case 3: return RGB( 56, 224, 200); // teal
	case 4: return RGB(255, 110, 199); // pink
	case 5: return RGB(104, 132, 255); // periwinkle
	case 6: return RGB(196, 116, 255); // orchid
	default:return RGB( 90, 214, 140); // mint
	}
}

// ---------------------------------------------------------------- background

void ui_bg(float w, float h, float t, float phase)
{
	float s = sinf(t * 0.35f + phase) * 0.5f + 0.5f;

	// Two palettes crossfaded over time: pink->purple and magenta->indigo.
	u32 tlA = RGB(255, 138, 205), trA = RGB(238,  99, 198);
	u32 blA = RGB(140,  70, 232), brA = RGB( 96,  42, 176);

	u32 tlB = RGB(255, 106, 173), trB = RGB(203,  92, 226);
	u32 blB = RGB(112,  58, 214), brB = RGB( 70,  30, 150);

	u32 tl = ui_lerp(tlA, tlB, s);
	u32 tr = ui_lerp(trA, trB, s);
	u32 bl = ui_lerp(blA, blB, s);
	u32 br = ui_lerp(brA, brB, s);

	C2D_DrawRectangle(0, 0, 0.0f, w, h, tl, tr, bl, br);

	// Drifting bubbles for depth.
	for (int i = 0; i < BUBBLE_COUNT; i++) {
		const Bubble* b = &s_bubbles[i];
		float span = h + b->r * 2.0f;
		float y = b->y - fmodf(t * b->speed + phase * 30.0f, span);
		if (y < -b->r) y += span;
		float x = b->x + sinf(t * 0.5f + (float)i) * b->drift;
		if (x > w + b->r) x -= w + b->r * 2.0f;
		C2D_DrawCircleSolid(x, y, 0.05f, b->r,
		                    RGBA(255, 255, 255, (u8)(b->alpha * 255.0f)));
	}

	// Soft vignette at the bottom so light text stays readable.
	C2D_DrawRectangle(0, h - 46.0f, 0.06f, w, 46.0f,
	                  RGBA(40, 8, 60, 0),  RGBA(40, 8, 60, 0),
	                  RGBA(40, 8, 60, 70), RGBA(40, 8, 60, 70));
}

// ----------------------------------------------------------------- text core

float ui_text_width(const char* s, float scale)
{
	if (!s || !*s) return 0.0f;

	float total = 0.0f;
	const char* p = s;
	char seg[UI_LINE_MAX];
	int  segLen = 0;
	int  len;

	while (*p) {
		int tile = emoji_tile(p, &len);
		if (tile >= 0) {
			if (segLen > 0) {
				seg[segLen] = '\0';
				C2D_TextBufClear(s_measureBuf);
				C2D_Text t;
				C2D_TextParse(&t, s_measureBuf, seg);
				float w = 0.0f, h = 0.0f;
				C2D_TextGetDimensions(&t, scale, scale, &w, &h);
				total += w;
				segLen = 0;
			}
			total += 30.0f * scale; // emoji advance = font cell height
		} else if (tile == -2) {
			// variation selector: no glyph, no advance
			if (segLen > 0) {
				seg[segLen] = '\0';
				C2D_TextBufClear(s_measureBuf);
				C2D_Text t;
				C2D_TextParse(&t, s_measureBuf, seg);
				float w = 0.0f, h = 0.0f;
				C2D_TextGetDimensions(&t, scale, scale, &w, &h);
				total += w;
				segLen = 0;
			}
		} else {
			for (int i = 0; i < len && segLen < UI_LINE_MAX - 1; i++) {
				seg[segLen++] = p[i];
			}
		}
		p += len;
	}
	if (segLen > 0) {
		seg[segLen] = '\0';
		C2D_TextBufClear(s_measureBuf);
		C2D_Text t;
		C2D_TextParse(&t, s_measureBuf, seg);
		float w = 0.0f, h = 0.0f;
		C2D_TextGetDimensions(&t, scale, scale, &w, &h);
		total += w;
	}
	return total;
}

float ui_line_height(float scale)
{
	// The system font's cell height is 30px at scale 1.0.
	return 30.0f * scale;
}

void ui_text(const char* s, float x, float y, float scale, u32 color)
{
	if (!s || !*s) return;

	float cx = x;
	const char* p = s;
	char seg[UI_LINE_MAX];
	int  segLen = 0;
	int  len;

	while (*p) {
		int tile = emoji_tile(p, &len);
		if (tile >= 0) {
			if (segLen > 0) {
				seg[segLen] = '\0';
				C2D_Text t;
				C2D_TextParse(&t, s_frameBuf, seg);
				C2D_TextOptimize(&t);
				float w = 0.0f, h = 0.0f;
				C2D_TextGetDimensions(&t, scale, scale, &w, &h);
				C2D_DrawText(&t, C2D_WithColor, cx, y, 0.5f, scale, scale, color);
				cx += w;
				segLen = 0;
			}
			float e = 30.0f * scale;
			if (s_emojiOk && tile < EMOJI_COUNT) {
				C2D_DrawImageAt(s_emojiImg[tile], cx, y - 2.0f * scale,
				                0.5f, NULL, scale, scale);
			}
			cx += e;
		} else if (tile == -2) {
			// variation selector: no glyph, no advance
			if (segLen > 0) {
				seg[segLen] = '\0';
				C2D_Text t;
				C2D_TextParse(&t, s_frameBuf, seg);
				C2D_TextOptimize(&t);
				float w = 0.0f, h = 0.0f;
				C2D_TextGetDimensions(&t, scale, scale, &w, &h);
				C2D_DrawText(&t, C2D_WithColor, cx, y, 0.5f, scale, scale, color);
				cx += w;
				segLen = 0;
			}
		} else {
			for (int i = 0; i < len && segLen < UI_LINE_MAX - 1; i++) {
				seg[segLen++] = p[i];
			}
		}
		p += len;
	}
	if (segLen > 0) {
		seg[segLen] = '\0';
		C2D_Text t;
		C2D_TextParse(&t, s_frameBuf, seg);
		C2D_TextOptimize(&t);
		C2D_DrawText(&t, C2D_WithColor, cx, y, 0.5f, scale, scale, color);
	}
}

void ui_text_shadow(const char* s, float x, float y, float scale, u32 color)
{
	if (!s || !*s) return;
	ui_text(s, x + 1.0f, y + 1.0f, scale, CLR_SHADOW);
	ui_text(s, x, y, scale, color);
}

void ui_text_center(const char* s, float cx, float y, float scale, u32 color)
{
	ui_text(s, cx - ui_text_width(s, scale) * 0.5f, y, scale, color);
}

void ui_text_center_shadow(const char* s, float cx, float y, float scale, u32 color)
{
	float x = cx - ui_text_width(s, scale) * 0.5f;
	ui_text_shadow(s, x, y, scale, color);
}

void ui_text_right(const char* s, float rx, float y, float scale, u32 color)
{
	ui_text(s, rx - ui_text_width(s, scale), y, scale, color);
}

// ------------------------------------------------------------------ wrapping

int ui_wrap(const char* s, float maxw, float scale,
            char lines[][UI_LINE_MAX], int maxLines)
{
	int nline = 0;
	if (!s || !*s || maxLines <= 0) return 0;

	char cur[UI_LINE_MAX];
	int  curLen = 0;
	cur[0] = '\0';

	const char* p = s;
	while (*p && nline < maxLines) {
		if (*p == '\n') {
			memcpy(lines[nline], cur, (size_t)curLen);
			lines[nline][curLen] = '\0';
			nline++;
			curLen = 0;
			cur[0] = '\0';
			p++;
			continue;
		}

		// Take the next whitespace-delimited word.
		const char* wstart = p;
		while (*p && *p != ' ' && *p != '\n') p++;
		int wlen = (int)(p - wstart);
		if (wlen >= UI_LINE_MAX) wlen = UI_LINE_MAX - 1;

		char cand[UI_LINE_MAX * 2];
		int  cl = 0;
		if (curLen > 0) {
			memcpy(cand, cur, (size_t)curLen);
			cl = curLen;
			cand[cl++] = ' ';
		}
		int room = UI_LINE_MAX - 1 - cl;
		int take = wlen < room ? wlen : room;
		if (take > 0) memcpy(cand + cl, wstart, (size_t)take);
		cl += take;
		cand[cl] = '\0';

		if (ui_text_width(cand, scale) <= maxw || curLen == 0) {
			// Fits, or the line is empty and we must place it regardless.
			if (ui_text_width(cand, scale) <= maxw) {
				memcpy(cur, cand, (size_t)cl);
				cur[cl] = '\0';
				curLen  = cl;
			} else {
				// Single word wider than the line: hard-break it per character.
				int used = 0;
				char part[UI_LINE_MAX];
				for (int i = 0; i < take && nline < maxLines; i++) {
					part[used]     = wstart[i];
					part[used + 1] = '\0';
					if (ui_text_width(part, scale) > maxw && used > 0) {
						part[used] = '\0';
						memcpy(lines[nline], part, (size_t)used + 1);
						nline++;
						part[0] = wstart[i];
						part[1] = '\0';
						used = 1;
					} else {
						used++;
					}
				}
				memcpy(cur, part, (size_t)used);
				cur[used] = '\0';
				curLen    = used;
			}
		} else {
			// Flush the current line and start a new one with this word.
			memcpy(lines[nline], cur, (size_t)curLen);
			lines[nline][curLen] = '\0';
			nline++;
			if (nline >= maxLines) break;
			memcpy(cur, wstart, (size_t)take);
			cur[take] = '\0';
			curLen    = take;
		}

		while (*p == ' ') p++;
	}

	if (curLen > 0 && nline < maxLines) {
		memcpy(lines[nline], cur, (size_t)curLen);
		lines[nline][curLen] = '\0';
		nline++;
	}
	return nline;
}

int ui_wrap_fit(const char* s, float maxw, float maxh,
                float scaleHi, float scaleLo,
                char lines[][UI_LINE_MAX], int maxLines, float* scaleOut)
{
	float scale = scaleHi;
	int   n     = 0;
	while (scale > scaleLo) {
		n = ui_wrap(s, maxw, scale, lines, maxLines);
		if (n * ui_line_height(scale) <= maxh) break;
		scale -= 0.05f;
	}
	if (scale <= scaleLo) {
		scale = scaleLo;
		n = ui_wrap(s, maxw, scale, lines, maxLines);
	}
	if (scaleOut) *scaleOut = scale;
	return n;
}

// ------------------------------------------------------------------ surfaces

void ui_panel(float x, float y, float w, float h, u32 fill)
{
	C2D_DrawRectSolid(x, y, 0.1f, w, h, fill);
}

void ui_panel_hi(float x, float y, float w, float h, u32 fill)
{
	C2D_DrawRectSolid(x, y, 0.1f, w, h, fill);
	// 1px inner highlight along the top edge reads as a bevel.
	C2D_DrawRectSolid(x, y, 0.11f, w, 1.0f, CLR_GLASS_HI);
}

void ui_bar(float x, float y, float w, float h, float pct, u32 fill, u32 track)
{
	if (pct < 0.0f) pct = 0.0f;
	if (pct > 1.0f) pct = 1.0f;
	C2D_DrawRectSolid(x, y, 0.1f, w, h, track);
	C2D_DrawRectSolid(x, y, 0.11f, w * pct, h, fill);
}

void ui_tile(float x, float y, float w, float h, u32 fill, bool pressed)
{
	float off = pressed ? 1.0f : 0.0f;
	// Drop shadow, then the face, then a top highlight.
	C2D_DrawRectSolid(x + 2.0f, y + 3.0f, 0.09f, w, h, RGBA(40, 8, 60, 90));
	C2D_DrawRectSolid(x, y + off, 0.1f, w, h, fill);
	C2D_DrawRectSolid(x, y + off, 0.11f, w, 2.0f, ui_lerp(fill, CLR_WHITE, 0.35f));
	C2D_DrawRectSolid(x, y + h - 3.0f + off, 0.11f, w, 3.0f, ui_lerp(fill, CLR_INK, 0.30f));
}

bool ui_hit(float x, float y, float w, float h, int tx, int ty)
{
	return (float)tx >= x && (float)tx < x + w &&
	       (float)ty >= y && (float)ty < y + h;
}
