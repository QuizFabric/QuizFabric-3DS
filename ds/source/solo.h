// Solo play for the QuizFabric DS client: fetch the quiz list from the
// server, pick one, play through its questions with local scoring, then
// show the results. Back buttons everywhere.
#pragma once

#include <stdbool.h>

void solo_enter(const char* host, int port);
void solo_step(void);        // per-frame: downloads and timers
void solo_draw_top(void);
void solo_draw_bottom(void);
bool solo_wants_exit(void);  // B pressed on a back-able screen
void solo_exit(void);
