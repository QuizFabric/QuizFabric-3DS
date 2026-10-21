// Shared thumbnail loader for the quiz lists. See thumb.c.
#pragma once

#include <3ds.h>
#include <citro2d.h>

// Sets the server and clears the pool. Call when a list mode starts.
void thumb_begin(const char* host, int port);

// Advances the background fetch; call once per frame while a list shows.
void thumb_step(void);

// Queues a GET for the slot (no-op when already loaded or queued). `path`
// must be a fully percent-encoded path on the server set by thumb_begin.
void thumb_request(int idx, const char* path);

// Draws the thumbnail fitted into x/y/w/h. While it has not loaded yet:
// a down-arrow download symbol (with a small progress bar) when the quiz
// has a banner on the server, or a white ? on dark gray when it has none.
void thumb_draw(int idx, float x, float y, float w, float h, bool has_image);

// True when the slot's texture is ready to draw.
bool thumb_loaded(int idx);

// Loads a .565 straight from the SD card into a slot (offline banners).
// No-op when the slot is already loaded or the file is missing.
void thumb_load_file(int idx, const char* path);

// Progress (0..1) of the fetch currently in flight, -1 when idle/unknown.
float thumb_current_progress(void);

// Frees every texture and cancels the active fetch.
void thumb_reset(void);
