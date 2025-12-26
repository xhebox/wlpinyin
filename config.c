#include "wlpinyin.h"

static xkb_keysym_t records[2];
bool im_toggle(struct xkb_state *xkb, xkb_keysym_t keysym, bool pressed) {
	UNUSED(xkb);
	records[1] = records[0];
	records[0] = keysym;
#ifndef NDEBUG
	wlpinyin_dbg("toggle %u, %u, %s", records[0], records[1],
							 pressed ? "pressed" : "release");
#endif
	return pressed == false && records[0] == XKB_KEY_Control_L &&
				 records[1] == XKB_KEY_Control_L;
}
