// Shared thumbnail loader for the quiz lists. One fetch at a time, so the
// 3DS Wi-Fi is never flooded; slots cache decoded textures with an LRU cap.
// The server sends banners already converted to the PICA200 tiled layout
// (max 64x64), so decoding is a straight C3D_TexUpload like question images.

#include "thumb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"
#include "ws.h"

#define THUMB_SLOTS      128  // covers solo/host quiz caps and steal results
#define THUMB_MAX_LOADED 24   // VRAM cap: 64x64 tiles are 16 KiB each
#define THUMB_PATH       400
#define THUMB_RETRY_MS   3000  // cooldown after a failed fetch

typedef struct {
    C3D_Tex   tex;
    C2D_Image img;
    Tex3DS_SubTexture sub;
    bool      loaded;
    bool      queued;
    u16       w, h;      // original image size for aspect-fit drawing
    u32       lastUse;   // frame stamp for eviction + on-screen animation
    u64       retryAt;   // earliest time for another attempt (ms)
    // Animated GIF thumbnails: frames are cycled in-place, but only while
    // the slot is on screen (lastUse is fresh).
    u8*       frames;
    int       framesN;
    int       delay;     // centiseconds per frame
    int       cur;
    u64       nextAt;
    int       frameBytes;
    char      path[THUMB_PATH];
} ThumbSlot;

static ThumbSlot s_slots[THUMB_SLOTS];
static int  s_queue[THUMB_SLOTS];
static int  s_qHead = 0, s_qTail = 0;
static int  s_active = -1;
static int  s_loaded = 0;
static u32  s_frame = 0;
static char s_host[80];
static int  s_port = 80;
static HttpGet* s_get = NULL;

void thumb_reset(void)
{
    if (s_get) {
        http_get_close(s_get);
        s_get = NULL;
    }
    s_active = -1;
    s_qHead = s_qTail = 0;
    for (int i = 0; i < THUMB_SLOTS; i++) {
        ThumbSlot* s = &s_slots[i];
        if (s->loaded) C3D_TexDelete(&s->tex);
        free(s->frames);
        s->frames = NULL;
        s->framesN = 0;
        s->loaded = false;
        s->queued = false;
        s->path[0] = '\0';
    }
    s_loaded = 0;
}

void thumb_begin(const char* host, int port)
{
    snprintf(s_host, sizeof(s_host), "%s", host ? host : "");
    s_port = port;
    thumb_reset();
}

void thumb_request(int idx, const char* path)
{
    if (idx < 0 || idx >= THUMB_SLOTS) return;
    ThumbSlot* s = &s_slots[idx];
    if (s->loaded || s->queued) return;
    if (osGetTime() < s->retryAt) return; // wait out the failure cooldown
    snprintf(s->path, sizeof(s->path), "%s", path ? path : "");
    if (!s->path[0]) return;
    s->queued = true;
    s_queue[s_qTail] = idx;
    s_qTail = (s_qTail + 1) % THUMB_SLOTS;
    if (s_qTail == s_qHead) s_qHead = (s_qHead + 1) % THUMB_SLOTS; // ring full
}

// Decodes the .565 payload into slot idx: either the static format
// (u16 w, u16 h + tiled RGBA8) or the animated format (u16 w, u16 h,
// u8 frame_count, u8 delay, then frame_count tiled RGBA8 payloads).
static bool thumb_decode(int idx, const char* body, int len)
{
    if (len < 8) return false;
    u16 w = (u16)((unsigned char)body[0] | ((unsigned char)body[1] << 8));
    u16 h = (u16)((unsigned char)body[2] | ((unsigned char)body[3] << 8));
    if (w < 1 || h < 1) return false;
    int pw = 8; while (pw < w) pw <<= 1;
    int ph = 8; while (ph < h) ph <<= 1;
    int frameBytes = pw * ph * 4;

    bool animated = (len == 6 + (int)(unsigned char)body[4] * frameBytes);
    if (len != 4 + frameBytes && !animated) return false;

    int framesN = 0;
    int delay = 10;
    const char* first = body + 4;
    u8* frames = NULL;
    if (animated) {
        framesN = (unsigned char)body[4];
        delay = (unsigned char)body[5];
        first = body + 6;
        if (framesN < 2 || framesN > 24 || delay == 0) return false;
        if ((size_t)framesN * (size_t)frameBytes > 1024 * 1024) return false;
        frames = (u8*)malloc((size_t)framesN * (size_t)frameBytes);
        if (!frames) return false;
        memcpy(frames, body + 6, (size_t)framesN * (size_t)frameBytes);
    }

    ThumbSlot* s = &s_slots[idx];
    if (s->loaded) {
        C3D_TexDelete(&s->tex);
        s->loaded = false;
        s_loaded--;
    }
    free(s->frames);
    s->frames = NULL;
    s->framesN = 0;

    // Evict the least recently used slot when the VRAM cap would overflow.
    if (s_loaded >= THUMB_MAX_LOADED) {
        u32 oldest = 0xFFFFFFFFu;
        int victim = -1;
        for (int i = 0; i < THUMB_SLOTS; i++) {
            if (s_slots[i].loaded && s_slots[i].lastUse < oldest) {
                oldest = s_slots[i].lastUse;
                victim = i;
            }
        }
        if (victim >= 0) {
            C3D_TexDelete(&s_slots[victim].tex);
            free(s_slots[victim].frames);
            s_slots[victim].frames = NULL;
            s_slots[victim].framesN = 0;
            s_slots[victim].loaded = false;
            s_loaded--;
        }
    }

    if (!C3D_TexInit(&s->tex, (u16)pw, (u16)ph, GPU_RGBA8)) {
        free(frames);
        return false;
    }
    C3D_TexSetFilter(&s->tex, GPU_LINEAR, GPU_NEAREST);
    C3D_TexSetWrap(&s->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexUpload(&s->tex, first);
    s->sub = (Tex3DS_SubTexture){ w, h, 0.0f, 1.0f,
                                  (float)w / (float)pw, 1.0f - (float)h / (float)ph };
    s->img = (C2D_Image){ &s->tex, &s->sub };
    s->w = w;
    s->h = h;
    s->loaded = true;
    s->lastUse = s_frame;
    s->frames = frames;
    s->framesN = framesN;
    s->delay = delay;
    s->cur = 0;
    s->nextAt = osGetTime() + (u64)delay * 10;
    s->frameBytes = frameBytes;
    s_loaded++;
    return true;
}

void thumb_step(void)
{
    s_frame++;
    // Animate loaded GIF thumbnails, but ONLY the ones drawn this frame:
    // off-screen thumbnails freeze (no wasted GPU uploads). thumb_draw
    // refreshes lastUse each frame the tile is visible.
    u64 now = osGetTime();
    for (int i = 0; i < THUMB_SLOTS; i++) {
        ThumbSlot* s = &s_slots[i];
        if (!s->loaded || s->framesN < 2) continue;
        if (s_frame - s->lastUse > 2) continue;
        if (now < s->nextAt) continue;
        s->cur = (s->cur + 1) % s->framesN;
        s->nextAt = now + (u64)s->delay * 10;
        C3D_TexUpload(&s->tex, s->frames + (size_t)s->cur * s->frameBytes);
        C3D_TexFlush(&s->tex);
    }
    if (s_active >= 0 && s_get) {
        int r = http_get_step(s_get);
        if (r == 0) return;
        int len = 0;
        char* body = (r == 1) ? http_get_take_body_len(s_get, &len) : NULL;
        http_get_close(s_get);
        s_get = NULL;
        int idx = s_active;
        s_active = -1;
        if (body) {
            thumb_decode(idx, body, len);
            free(body);
        }
        s_slots[idx].queued = false;
        if (!s_slots[idx].loaded) {
            s_slots[idx].retryAt = osGetTime() + THUMB_RETRY_MS;
        }
    }
    while (s_active < 0 && s_qHead != s_qTail) {
        int idx = s_queue[s_qHead];
        s_qHead = (s_qHead + 1) % THUMB_SLOTS;
        ThumbSlot* s = &s_slots[idx];
        if (!s->queued || s->loaded) {
            s->queued = false;
            continue;
        }
        s_get = http_get_begin(s_host, s_port, s->path, 8000);
        if (s_get) {
            s_active = idx;
            return;
        }
        s->queued = false;
        s->retryAt = osGetTime() + THUMB_RETRY_MS;
    }
}

// Download progress for a slot: 1 when loaded, -1 when idle/unknown.
float thumb_progress(int idx)
{
    if (idx >= 0 && idx < THUMB_SLOTS && s_slots[idx].loaded) return 1.0f;
    if (idx == s_active && s_get) return http_get_progress(s_get);
    return -1.0f;
}

bool thumb_loaded(int idx)
{
    return idx >= 0 && idx < THUMB_SLOTS && s_slots[idx].loaded;
}

void thumb_load_file(int idx, const char* path)
{
    if (idx < 0 || idx >= THUMB_SLOTS) return;
    ThumbSlot* s = &s_slots[idx];
    if (s->loaded) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 8 || sz > 1024 * 1024) {
        fclose(f);
        return;
    }
    char* buf = (char*)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    thumb_decode(idx, buf, (int)sz);
    free(buf);
}

float thumb_current_progress(void)
{
    if (s_active >= 0 && s_get) return http_get_progress(s_get);
    return -1.0f;
}

void thumb_draw(int idx, float x, float y, float w, float h, bool has_image)
{
    if (idx >= 0 && idx < THUMB_SLOTS && s_slots[idx].loaded) {
        ThumbSlot* s = &s_slots[idx];
        s->lastUse = s_frame;
        float scl = h / (float)s->h;
        if ((float)s->w * scl > w) scl = w / (float)s->w;
        float dw = (float)s->w * scl;
        float dh = (float)s->h * scl;
        C2D_DrawImageAt(s->img, x + (w - dw) / 2.0f + ui_parallax(PAR_IMAGE),
                        y + (h - dh) / 2.0f, 0.5f, NULL, scl, scl);
        return;
    }
    C2D_DrawRectSolid(x + ui_parallax(PAR_PANEL), y, 0.5f, w, h, RGB(59, 59, 67));
    if (has_image) {
        // Downloading or waiting: a white down-arrow download symbol
        // (stem + triangle) above the progress bar along the bottom. The
        // bar owns the last 6px, so the arrow never gets covered by it.
        bool bar = idx >= 0 && idx < THUMB_SLOTS && s_slots[idx].queued && h >= 14.0f;
        float ab = y + (bar ? h - 6.0f : h); // bottom of the arrow area
        float cx = x + w / 2.0f + ui_parallax(PAR_PANEL);
        float stemTop = y + (ab - y) * 0.12f;
        float stemH = (ab - y) * 0.40f;
        C2D_DrawRectSolid(cx - 1.5f, stemTop, 0.5f, 3.0f, stemH, CLR_WHITE);
        float ty = stemTop + stemH;
        float half = w * 0.22f;
        if (half < 4.0f) half = 4.0f;
        if (half > 7.0f) half = 7.0f;
        C2D_DrawTriangle(cx - half, ty, CLR_WHITE,
                         cx + half, ty, CLR_WHITE,
                         cx, ty + 6.0f, CLR_WHITE,
                         0.5f);
        if (bar) {
            float p = thumb_progress(idx);
            float bx = x + 3.0f, bw = w - 6.0f, by = y + h - 5.0f;
            if (p >= 0.0f) {
                if (p > 1.0f) p = 1.0f;
                ui_bar(bx, by, bw, 3.0f, p, CLR_WHITE, RGB(46, 16, 70));
            } else {
                float pct = (float)((osGetTime() / 250) % 1000) / 1000.0f;
                ui_bar(bx, by, bw, 3.0f, pct, CLR_WHITE, RGB(46, 16, 70));
            }
        }
    } else {
        // The quiz definitely has no banner: white ? on the dark gray box.
        ui_text_center("?", x + w / 2.0f, y + h / 2.0f - 9.0f, 0.6f, CLR_WHITE);
    }
}
