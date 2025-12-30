#include "wlpinyin.h"

// Modifier masks
const unsigned int ShiftMask = (1 << 5);
const unsigned int ControlMask = (1 << 6);
const unsigned int Mod1Mask = (1 << 0);
const unsigned int Mod4Mask = (1 << 3);

/*
 * Use XKB key names directly. Find them at:
 * /usr/include/xkbcommon/xkbcommon-keysyms.h
 * or use: xev and look for "keysym" values
 * examples:
 * const char *toggle_sequence_str[] = {"Super_L space"};
 * const char *toggle_sequence_str[] = {"Control_L Shift_L space"};
 * const char *toggle_sequence_str[] = {"Alt_L space"};
 * const char *toggle_sequence_str[] = {"Control_L c"};
 */

const char *toggle_sequence_str[] = {
	"Control_L space"
};

const size_t toggle_sequence_str_len = sizeof(toggle_sequence_str) / sizeof(toggle_sequence_str[0]);

#ifdef ENABLE_POPUP
const float popup_bg_rgba[4] = {0.25, 0.25, 0.27, 0.95};
const float popup_hl_rgba[4] = {0.3, 0.5, 0.8, 1.0};
const float popup_txt_rgba[4] = {0.95, 0.95, 0.95, 1.0};
const char *popup_font = "Sans 12";
const int popup_spacing = 8;
#endif
