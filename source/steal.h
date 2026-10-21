// Steal-a-quiz (beta): imports a public Kahoot quiz by ID, link, or search
// term via the server's /api/stealquiz, then shows the saved name, lets the
// player rename it, and saves (/api/renamequiz).
#pragma once

#include <3ds.h>

#include <stdbool.h>

void steal_start(const char* host, int port);
void steal_step(void);
void steal_input(u32 kDown, touchPosition* touch, bool touched);
void steal_draw_top(void);
void steal_draw_bottom(void);

bool steal_wants_exit(void);
void steal_exit(void);
