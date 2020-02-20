#ifndef WLPINYIN_H
#define WLPINYIN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"

struct engine;

struct wlpinyin_key {
	xkb_keysym_t keysym;
	uint32_t keycode;
	bool pressed;
};

struct wlpinyin_state {
	struct wl_display *display;

	struct wl_seat *seat;
	struct zwp_input_method_manager_v2 *input_method_manager;
	struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;

	struct zwp_input_method_v2 *input_method;
	struct zwp_input_method_keyboard_grab_v2 *input_method_keyboard_grab;
	struct zwp_virtual_keyboard_v1 *virtual_keyboard;

	bool im_activate;
	bool im_forwarding;
	bool im_exit;
	bool im_only_modifier;

	uint32_t im_serial;
	uint32_t im_mods;
	int im_event_fd;
	int im_candidate_len;

	int im_repeat_timer;
	int32_t im_repeat_delay;
	int32_t im_repeat_rate;
	uint32_t im_repeat_key;
	uint32_t im_repeat_times;

	char *im_prefix;
	struct engine *engine;

	uint32_t *im_pressed;
	int im_pressed_num;
	int im_pressed_cap;

	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
};

extern void noop();

bool im_toggle(bool only_modifier,
							 struct xkb_state *state,
							 xkb_keysym_t keysym);

int im_setup(struct wlpinyin_state *state);
int im_event_fd(struct wlpinyin_state *state);
int im_repeat_timerfd(struct wlpinyin_state *state);
void im_repeat(struct wlpinyin_state *state, uint64_t times);
void im_exit(struct wlpinyin_state *state);
bool im_running(struct wlpinyin_state *state);
void im_handle(struct wlpinyin_state *state);
void im_destroy(struct wlpinyin_state *state);

struct engine *im_engine_new();
void im_engine_free(struct engine *);

const char *im_engine_raw_get(struct engine *);
const char *im_engine_preedit_get(struct engine *);
int im_engine_candidate_len(struct engine *);
const char *im_engine_candidate_get(struct engine *, int);
const char *im_engine_commit_text(struct engine *);
bool im_engine_key(struct engine *, xkb_keysym_t, xkb_mod_mask_t);
bool im_engine_page(struct engine *, bool next);
bool im_engine_cursor(struct engine *, bool right);
bool im_engine_delete(struct engine *, bool del);
bool im_engine_candidate_choose(struct engine *, int);

void im_engine_activate(struct engine *);
// used to ungrab unneeded keys when it's not in selection
bool im_engine_activated(struct engine *);
void im_engine_deactivate(struct engine *);

#define wlpinyin_err(fmt, ...) \
	fprintf(stderr, "[%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

#ifndef NDEBUG
#define wlpinyin_dbg(fmt, ...) \
	fprintf(stderr, "[%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define wlpinyin_dbg(fmt, ...)
#endif

#define UNUSED(x) (void)x;

#endif
