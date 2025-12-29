#include <xkbcommon/xkbcommon.h>

/* toggle key choice + whether need to press it twice */
static unsigned int TOGGLE_KEY = XKB_KEY_Control_L;
static bool REQUIRE_DOUBLE_TOGGLE = 1;

#ifdef ENABLE_POPUP
/* pop-up rendering options */
static float POPUP_BG_RGBA[] = {0.25, 0.25, 0.27, 0.95};
static float POPUP_HL_RGBA[] = {0.3, 0.5, 0.8, 1.0};
static float POPUP_TXT_RGBA[] = {0.95, 0.95, 0.95, 1.0};
static char  POPUP_FONT[] = "Sans 12";
#endif
