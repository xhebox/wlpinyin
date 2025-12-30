#include "wlpinyin.h"

static xkb_keysym_t records[2];
bool im_toggle(struct xkb_state *xkb, xkb_keysym_t keysym, bool pressed) {
	UNUSED(xkb);
	records[1] = records[0];
	records[0] = keysym;
	return pressed == false && records[0] == XKB_KEY_Control_L &&
				 records[1] == XKB_KEY_Control_L;
}

const float popup_bg_rgba[4] = {0.25, 0.25, 0.27, 0.95};
const float popup_hl_rgba[4] = {0.3, 0.5, 0.8, 1.0};
const float popup_txt_rgba[4] = {0.95, 0.95, 0.95, 1.0};
const char *popup_font = "Sans 12";
const int popup_spacing = 8;
