#ifndef WLPINYIN_H
#define WLPINYIN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

// user config
bool im_toggle(struct xkb_state *xkb, xkb_keysym_t keysym, bool pressed);
extern bool default_activation;

// internal
struct engine;

struct wlpinyin_state {
	int signalfd;
	int timerfd;
	struct wl_display *display;

	struct wl_seat *seat;
	struct zwp_input_method_manager_v2 *input_method_manager;
	struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct wl_compositor *compositor;

	/*
	struct wl_surface *popup_surface_wl;
	struct zwp_input_popup_surface_v2 *popup_surface;
	*/

	struct zwp_input_method_v2 *input_method;
	struct zwp_input_method_keyboard_grab_v2 *input_method_keyboard_grab;
	struct zwp_virtual_keyboard_v1 *virtual_keyboard;

	uint32_t im_serial;

	uint32_t im_repeat_delay;
	uint32_t im_repeat_rate;
	uint32_t im_repeat_key;

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

bool im_engine_bypass(struct engine *);
int im_engine_key(struct engine *, xkb_keysym_t, xkb_mod_mask_t);

typedef struct predit {
	int cursor;
	int start;
	int end;
	char *text;
} preedit_t;

typedef struct candidate {
	int page_no;
	int highlighted_candidate_index;
	int num_candidates;
} candidate_t;

preedit_t im_engine_preedit(struct engine *);
candidate_t im_engine_candidate(struct engine *);
const char *im_engine_candidate_get(struct engine *, int);
const char *im_engine_commit_text(struct engine *);
void im_engine_toggle(struct engine *);
void im_engine_reset(struct engine *);

#define wlpinyin_err(fmt, ...) \
	fprintf(stderr, "[%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

#ifndef NDEBUG
#define wlpinyin_dbg(fmt, ...) \
	fprintf(stderr, " [%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define wlpinyin_dbg(fmt, ...)
#endif

#define UNUSED(x) (void)x;

#endif
