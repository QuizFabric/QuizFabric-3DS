// Framebuffer graphics for the QuizFabric DS build.
//   top:    16-bit 256x192 bitmap (R5 G5 B5, red low)
//   bottom:  8-bit 256x192 bitmap (palette: gradient ramp + UI colours)
// Primitives are plain software fills + the embedded 8x8 font. See gfx.c.
#pragma once

#include <nds.h>

#include <stdbool.h>

#define DS_TOP_W 256
#define DS_TOP_H 192
#define DS_BOT_W 256
#define DS_BOT_H 192

// 8-bit palette indices used throughout the UI.
enum {
	COL_BG = 0,     // dark purple background
	COL_PANEL,      // glass panel
	COL_TEXT,       // white text
	COL_DIM,        // soft white
	COL_GOLD,       // selection gold
	COL_PINK,       // accent pink
	COL_GOOD,       // green
	COL_BAD,        // red
	COL_INK,        // dark ink on bright buttons
	COL_BAR,        // progress track
	COL_COUNT
};

void gfx_init(void);
void gfx_frame_begin(void);
void gfx_frame_end(void);

// Per-frame helpers (operate on the CURRENT target).
void gfx_set_target_top(void);
void gfx_set_target_bottom(void);

void gfx_clear(u8 idx);
void gfx_rect(int x, int y, int w, int h, u8 idx);
void gfx_bar(int x, int y, int w, int h, float pct, u8 fill, u8 track);
void gfx_text(int x, int y, u8 col, const char* s);            // 1x scale
void gfx_text_scale(int x, int y, u8 col, const char* s, int scale);
int  gfx_text_width(const char* s, int scale);
// Wraps `s` into `lines` (up to maxLines), shrinking `*scale` until it fits
// maxw x maxh. Returns the line count; each line is NUL-terminated.
int  gfx_wrap_fit(const char* s, int maxw, int maxh, int scaleHi, int scaleLo,
                  char lines[][96], int maxLines, int* scaleOut);
void gfx_text_center(int cx, int y, u8 col, const char* s, int scale);

// Raw 15-bit image blit, 1:1 pixels, DS-native RGB555 (red low) - the
// format the server sends low-res question images in for the DS client.
void gfx_blit555(int x, int y, int w, int h, const u16* px);

// Boot diagnostics (dev builds): fills the sub screen with a big 3-digit
// code using direct VRAM/palette writes, no other dependencies.
void gfx_dbg(int code);

// Small seconds counter in the sub screen's bottom-right corner.
void gfx_dbg_corner(int seconds);
