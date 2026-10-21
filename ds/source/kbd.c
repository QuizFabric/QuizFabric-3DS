// Touch-screen keyboard for the DS build. See kbd.h.
//
// Layout (bottom screen, 256x192):
//   row 0: text strip
//   rows 1-3: QWERTY (10 keys each)
//   row 4: 123 + SPACE + BS + ENTER + CANCEL

#include "kbd.h"

#include <string.h>

#include "gfx.h"

#define KBD_MAX 96

typedef struct {
	int x, y, w, h;
	const char* label;
} Key;

static Key s_keys[36];
static int  s_nKeys = 0;

static void add_key(int x, int y, int w, int h, const char* label)
{
	Key* k = &s_keys[s_nKeys++];
	k->x = x; k->y = y; k->w = w; k->h = h; k->label = label;
}

void kbd_init(void)
{
	s_nKeys = 0;
	static const char* rows[] = {
		"QWERTYUIOP",
		"ASDFGHJKL:",
		"ZXCVBNM.-",
	};
	for (int r = 0; r < 3; r++) {
		const char* row = rows[r];
		for (int c = 0; row[c]; c++) {
			char label[2] = { row[c], '\0' };
			add_key(2 + c * 25, 34 + r * 26, 23, 24, label);
		}
	}
	add_key(2, 112, 50, 24, "123");
	add_key(54, 112, 62, 24, "SPACE");
	add_key(118, 112, 44, 24, "BS");
	add_key(164, 112, 44, 24, "ENT");
	add_key(210, 112, 44, 24, "CAN");
}

void kbd_draw(const char* text)
{
	gfx_set_target_bottom();
	gfx_clear(COL_BG);
	gfx_rect(0, 0, DS_BOT_W, 30, COL_PANEL);
	if (text && *text) gfx_text(4, 4, COL_TEXT, text);
	else gfx_text(4, 4, COL_DIM, "type here...");
	for (int i = 0; i < s_nKeys; i++) {
		Key* k = &s_keys[i];
		gfx_rect(k->x, k->y, k->w, k->h, COL_PANEL);
		gfx_text_center(k->x + k->w / 2, k->y + 7, COL_TEXT, k->label, 1);
	}
}

bool kbd_touch(touchPosition* t, char* buf, int bufsz, bool* done)
{
	int tx = t->px, ty = t->py;
	int len = (int)strlen(buf);
	for (int i = 0; i < s_nKeys; i++) {
		Key* k = &s_keys[i];
		if (tx < k->x || tx >= k->x + k->w || ty < k->y || ty >= k->y + k->h) continue;
		if (!strcmp(k->label, "ENT")) {
			*done = true;
			return true;
		}
		if (!strcmp(k->label, "CAN")) {
			buf[0] = '\0';
			*done = false;
			return true;
		}
		if (!strcmp(k->label, "BS")) {
			if (len > 0) buf[len - 1] = '\0';
			return false;
		}
		if (!strcmp(k->label, "SPACE")) {
			if (len + 1 < bufsz) strcat(buf, " ");
			return false;
		}
		if (len + 2 < bufsz) {
			char c = k->label[0];
			buf[len] = (c == ':' ? ' ' : c);
			buf[len + 1] = '\0';
		}
		return false;
	}
	return false;
}

void kbd_tick(void)
{
	(void)0; // simple tap keyboard for now
}
