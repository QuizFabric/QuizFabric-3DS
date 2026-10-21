// Touch-screen keyboard for the DS build (the DS has no system keyboard).
#pragma once

#include <nds.h>

#include <stdbool.h>

// Initialises the keyboard UI (call once).
void kbd_init(void);

// Draws the keyboard with the current text in the top strip.
void kbd_draw(const char* text);

// Feed a touch (already read); returns true when input is finished.
// `buf` holds the result on return; `done` is set for CONFIRM, false for
// CANCEL.
bool kbd_touch(touchPosition* t, char* buf, int bufsz, bool* done);

// Same but for a held stylus (repeat keys while held).
void kbd_tick(void);
