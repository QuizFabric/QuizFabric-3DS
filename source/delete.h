// Delete-a-quiz (beta): lists the quizzes this IP owns (uploads/steals) via
// /api/myquizzes and deletes the chosen one via /api/deletequiz - the same
// ownership rule as the web delete page.
#pragma once

#include <3ds.h>

#include <stdbool.h>

void delete_start(const char* host, int port);
void delete_step(void);
void delete_input(u32 kDown, touchPosition* touch, bool touched);
void delete_draw_top(void);
void delete_draw_bottom(void);

bool delete_wants_exit(void);
void delete_exit(void);
