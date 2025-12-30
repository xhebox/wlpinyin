#ifndef WLPINYIN_H
#define WLPINYIN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#ifdef ENABLE_POPUP
#include <pango/pango.h>
#endif

typedef struct {
	xkb_keysym_t keysym;
	bool pressed;
	unsigned int modmask;
} key_sequence_event_t;

extern const char *toggle_sequence_str[];
extern const size_t toggle_sequence_str_len;

extern const unsigned int ShiftMask;
extern const unsigned int ControlMask;
extern const unsigned int Mod1Mask;
extern const unsigned int Mod4Mask;

bool im_toggle(struct xkb_state *xkb, xkb_keysym_t keysym, bool pressed);
extern bool default_activation;

#ifdef ENABLE_POPUP
extern const float popup_bg_rgba[4];
extern const float popup_hl_rgba[4];
extern const float popup_txt_rgba[4];
extern const char *popup_font;
extern const int popup_spacing;
#endif

// internal
struct engine;

struct wlpinyin_state {
	int signalfd;
	struct wl_display *display;

	struct wl_seat *seat;
	struct zwp_input_method_manager_v2 *input_method_manager;
	struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct wl_compositor *compositor;
	struct wl_shm *wl_shm;

#ifdef ENABLE_POPUP
	struct wl_surface *popup_surface;
	struct zwp_input_popup_surface_v2 *popup_surface_v2;
	int shm_pool_fd;
	struct wl_shm_pool *shm_pool;
	int shm_size;
	struct wl_buffer *shm_buffer;
	void *popup_data;
	PangoContext *popup_pango_ctx;
	PangoLayout *popup_pango_layout;
	bool frame_callback_done;
	bool pending_render;
#endif

	struct zwp_input_method_v2 *input_method;
	struct zwp_input_method_keyboard_grab_v2 *input_method_keyboard_grab;
	struct zwp_virtual_keyboard_v1 *virtual_keyboard;

	uint32_t im_serial;

	bool im_activated;

	struct engine *engine;

	struct xkb_context *xkb_context;
	char *xkb_keymap_string;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
};

struct wlpinyin_state *im_setup(int signalfd, struct wl_display *display);
int im_loop(struct wlpinyin_state *state);
int im_destroy(struct wlpinyin_state *state);

struct engine *im_engine_new();
void im_engine_free(struct engine *);

typedef struct {
	char *text;
	int begin;
	int end;
} im_preedit_t;

typedef struct {
	int page_no;
	int highlighted_index;
	int page_size;
} im_context_t;

void im_engine_cand_begin(struct engine *engine, int off);
const char *im_engine_cand_get(struct engine *engine);
bool im_engine_cand_next(struct engine *engine);
void im_engine_cand_end(struct engine *engine);
const char *im_engine_commit(struct engine *engine);
im_preedit_t im_engine_preedit(struct engine *);
im_context_t im_engine_context(struct engine *);
bool im_engine_key(struct engine *, xkb_keysym_t, xkb_mod_mask_t);
void im_engine_toggle(struct engine *);
void im_engine_reset(struct engine *);

int im_panel_init(struct wlpinyin_state *);
int im_panel_update(struct wlpinyin_state *);
void im_panel_destroy(struct wlpinyin_state *);

#define wlpinyin_err(fmt, ...)                                     \
	fprintf(stderr, "[%*s:%*d] " fmt "\n", 8, __FILE__, 3, __LINE__, \
					##__VA_ARGS__)

#ifndef NDEBUG
#define wlpinyin_dbg(fmt, ...)                                      \
	fprintf(stderr, " [%*s:%*d] " fmt "\n", 8, __FILE__, 3, __LINE__, \
					##__VA_ARGS__)
#else
#define wlpinyin_dbg(fmt, ...)
#endif

#define UNUSED(x) (void)x

#endif
