// Host control panel for the QuizFabric 3DS client.
//
// Enter the 4-digit control code shown on the host screen to drive the game
// from the 3DS: watch answers land in real time, reveal answers, advance
// questions, toggle autoplay and kick players. Speaks the same socket.io
// events as the web control page (/control/<code>).
#pragma once

#include <3ds.h>

#include <stdbool.h>

// Resolves the control code (one small blocking HTTP GET) and opens the
// control socket. Returns false and fills `err` on failure.
bool ctrl_start(const char* host, int port, const char* controlCode,
                char* err, int errSz);

// Advances the connection and handles events. Call once per frame while
// ctrl_is_active() is true.
void ctrl_step(void);

void ctrl_input(u32 kDown, touchPosition* touch, bool touched);
void ctrl_draw_top(void);
void ctrl_draw_bottom(void);

bool ctrl_is_active(void);

// True once the user asked to leave or the connection died; the caller should
// ctrl_exit() and return to the menu.
bool ctrl_wants_exit(void);

// Why the panel wants to leave, or "" for a user-requested exit.
const char* ctrl_error(void);

void ctrl_exit(void);
