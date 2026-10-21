// Solo quiz mode (beta): browse the quiz list on the server and play one
// locally - no game code, no other players. Questions, answers and correct
// indices come from /api/quiz/<name>; scoring matches the server
// (max(0, 1000 - seconds*50)).
#pragma once

#include <3ds.h>

#include <stdbool.h>

// Begins solo mode: fetches the quiz list in the background. The caller sets
// the app state to ST_SOLO_LIST and routes drawing/input here.
void solo_start(const char* host, int port);

// Advances background fetches. Call once per frame while in a solo state.
void solo_step(void);

void solo_input(u32 kDown, touchPosition* touch, bool touched);
void solo_draw_top(void);
void solo_draw_bottom(void);

// True once the user asked to leave solo mode entirely.
bool solo_wants_exit(void);
void solo_exit(void);

// Image helpers owned by main.c, reused by solo mode (the same .565 fetch +
// GPU texture used for online questions).
void img_request(const char* url);
// Builds the SD card cache paths for a .565 question-image URL (used by the
// pre-cache mode as well). Returns false for non-upload URLs.
bool img_cache_paths(const char* url565, char* quizOut, int quizSz,
                     char* cacheOut, int cacheSz, char* dlOut, int dlSz);
// Like img_request, but the path is used exactly as given: no .565
// extension swap and no space encoding (the caller built a fully encoded
// server path - used for the Kahoot cover preview proxy).
void img_request_raw(const char* path);
bool img_is_loaded(void);
// True when the image being downloaded is a GIF (label says so).
bool img_is_gif(void);
// True while replaying from the SD card cache (label says so).
bool img_is_from_cache(void);
bool img_pending(void);
float img_progress(void);
void img_draw_at(float cx, float topY, float maxw, float maxh);
// Offline mode: cached images only, no network downloads.
void img_set_offline(bool on);
void solo_offline_mode(bool on);
