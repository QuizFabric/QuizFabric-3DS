// Hosting from the 3DS (beta): browse the quiz list, create a hosted game on
// the server via /api/host/<name>, show the game PIN + control code, and jump
// into the control panel to run it.
#pragma once

#include <3ds.h>

#include <stdbool.h>

void host_start(const char* host, int port);
void host_step(void);
void host_input(u32 kDown, touchPosition* touch, bool touched);
void host_draw_top(void);
void host_draw_bottom(void);

bool        host_wants_exit(void);     // user wants back to the menu
bool        host_wants_control(void);  // user wants to open the control panel
const char* host_control_code(void);   // valid when host_wants_control()
void        host_exit(void);
