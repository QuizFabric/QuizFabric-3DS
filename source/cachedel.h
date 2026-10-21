// Offline menu: delete cached quizzes from the SD card. See cachedel.c.
#pragma once

#include <3ds.h>

#include <stdbool.h>

void cachedel_start(void);
void cachedel_step(void);
void cachedel_input(u32 kDown, touchPosition* touch, bool touched);
void cachedel_draw_top(void);
void cachedel_draw_bottom(void);

bool cachedel_wants_exit(void);
void cachedel_exit(void);
