// Drawing helpers for the QuizFabric 3DS client: animated pink/purple
// gradient background, text measurement/wrapping, panels and buttons.
#pragma once

#include <3ds.h>
#include <citro2d.h>

#include <stdbool.h>

#define TOP_W 400.0f
#define BOT_W 320.0f
#define SCR_H 240.0f

#define UI_LINE_MAX  160
#define UI_MAX_LINES 12

#define RGB(r, g, b)      C2D_Color32((r), (g), (b), 255)
#define RGBA(r, g, b, a)  C2D_Color32((r), (g), (b), (a))

// Palette
#define CLR_WHITE     RGB(255, 255, 255)
#define CLR_SOFT      RGBA(255, 255, 255, 210)
#define CLR_DIM       RGBA(255, 255, 255, 150)
#define CLR_INK       RGB(46, 16, 70)
#define CLR_GLASS     RGBA(255, 255, 255, 46)
#define CLR_GLASS_HI  RGBA(255, 255, 255, 92)
#define CLR_SHADOW    RGBA(40, 8, 60, 120)
#define CLR_GOOD      RGB(58, 214, 160)
#define CLR_BAD       RGB(255, 92, 116)
#define CLR_GOLD      RGB(255, 208, 92)

void  ui_init(void);
void  ui_exit(void);

// Clears the per-frame text buffer. Call once per frame before drawing.
void  ui_begin_frame(void);

// Stereoscopic 3D: each stereo pass sets the eye (-1 = left, 0 = 2D,
// +1 = right) and the slider strength (0..1; pass 0 with eye 0 for 2D).
// Primitives shift their x by ui_parallax(depth), where depth is the
// layer's parallax in pixels at full strength, so layers sit at different
// distances behind (positive) or in front of (negative) the screen, and
// the whole effect scales with how high the 3D slider is.
void  ui_stereo_set(int eye, float strength);
float ui_parallax(float depth);

#define PAR_BG    6.0f // gradient + bubbles: farthest away
#define PAR_PANEL 3.0f // glass panels, tiles, bars, outlines
#define PAR_IMAGE -2.0f // question images / banners: pop out slightly

// Animated gradient plus drifting bubbles. `t` is seconds since start;
// `phase` offsets the animation so the two screens are not identical.
void  ui_bg(float w, float h, float t, float phase);

float ui_text_width(const char* s, float scale);
float ui_line_height(float scale);

void  ui_text(const char* s, float x, float y, float scale, u32 color);
void  ui_text_shadow(const char* s, float x, float y, float scale, u32 color);
void  ui_text_center(const char* s, float cx, float y, float scale, u32 color);
void  ui_text_center_shadow(const char* s, float cx, float y, float scale, u32 color);
void  ui_text_right(const char* s, float rx, float y, float scale, u32 color);

// Greedy word wrap honouring embedded newlines. Returns the line count.
int   ui_wrap(const char* s, float maxw, float scale,
              char lines[][UI_LINE_MAX], int maxLines);

// Shrinks `scale` until the wrapped text fits `maxw` x `maxh`, then wraps.
// Returns the line count and writes the chosen scale to *scaleOut.
int   ui_wrap_fit(const char* s, float maxw, float maxh,
                  float scaleHi, float scaleLo,
                  char lines[][UI_LINE_MAX], int maxLines, float* scaleOut);

void  ui_panel(float x, float y, float w, float h, u32 fill);
void  ui_panel_hi(float x, float y, float w, float h, u32 fill);
void  ui_bar(float x, float y, float w, float h, float pct, u32 fill, u32 track);

// Filled tile with a darker bottom edge; `pressed` sinks it by a pixel.
void  ui_tile(float x, float y, float w, float h, u32 fill, bool pressed);

bool  ui_hit(float x, float y, float w, float h, int tx, int ty);

u32   ui_lerp(u32 a, u32 b, float t);
u32   ui_fade(u32 c, float a);

// Tile colours for answer options, cycled by index.
u32   ui_answer_color(int i);
