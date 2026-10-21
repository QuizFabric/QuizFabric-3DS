// Pre-cache a quiz's photos and GIFs onto the SD card (X on a quiz in the
// solo/host lists). See precache.c.
#pragma once

#include <3ds.h>

#include <stdbool.h>

// Starts caching every question image of `quizName` on the server. Call once
// when the mode opens; drive with precache_step() every frame.
void precache_begin(const char* host, int port, const char* quizName);

// X+SELECT: deletes EVERYTHING cached and re-caches all quizzes from scratch.
void precache_begin_all(const char* host, int port);

void precache_step(void);
void precache_input(u32 kDown, touchPosition* touch, bool touched);
void precache_draw_top(void);
void precache_draw_bottom(void);

// True once the user asked to leave (finished, error, or cancelled).
bool precache_wants_back(void);
void precache_end(void);
