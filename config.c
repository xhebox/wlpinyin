#include "wlpinyin.h"

bool default_activation = true;

bool im_toggle(struct xkb_state *xkb, xkb_keysym_t keysym) {
	bool only_ctrl = xkb_state_mod_names_are_active(
											 xkb, XKB_STATE_MODS_EFFECTIVE, XKB_STATE_MATCH_ANY,
											 XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CAPS, XKB_MOD_NAME_ALT,
											 XKB_MOD_NAME_NUM, XKB_MOD_NAME_LOGO, NULL) == 0;
	return only_ctrl && keysym == XKB_KEY_Control_L;
}
