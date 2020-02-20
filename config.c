#include "wlpinyin.h"

bool im_toggle(bool only_modifier,
							 struct xkb_state *state,
							 xkb_keysym_t keysym) {
	return only_modifier && keysym == XKB_KEY_Control_L;
}
