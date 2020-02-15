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

enum wlpinyin_modifier {
	wlpinyin_alt = 0,
	wlpinyin_ctrl,
	wlpinyin_shift,
	wlpinyin_caps,
	wlpinyin_win,
	wlpinyin_num,
	wlpinyin_mod_max,
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

	uint32_t im_serial;
	uint32_t im_mods;
	int im_event_fd;

	int im_repeat_timer;
	int32_t im_repeat_delay;
	int32_t im_repeat_rate;
	uint32_t im_repeat_key;
	int im_candidate_num;
	int im_candidate_page;

	const char *im_aux_text;
	const char *im_cand_text[9];

	char *im_buf;
	size_t im_buflen;
	size_t im_bufcap;
	size_t im_bufpos;
	void *engine;

	struct wl_list im_pending_keys;
	struct wl_list im_unreleased_keys;

	char *xkb_keymap_string;
	int xkb_mods_index[wlpinyin_mod_max];
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
};

extern void noop();

void im_setup(struct wlpinyin_state *state);

int im_event_fd(struct wlpinyin_state *state);

int im_repeat_timerfd(struct wlpinyin_state *state);

void im_repeat(struct wlpinyin_state *state, uint64_t times);

void im_exit(struct wlpinyin_state *state);

bool im_running(struct wlpinyin_state *state);

void im_handle(struct wlpinyin_state *state);

void im_destroy(struct wlpinyin_state *state);

void *im_engine_new();
void im_engine_free(void *);

const char *im_engine_aux_get(void *, const char*);
void im_engine_aux_free(void *, const char *);
void im_engine_parse(void *, const char*);
const char *im_engine_candidate_get(void *, int);
void im_engine_candidate_free(void *, const char *);
size_t im_engine_candidate_choose(void *, int);

// maybe called even engined is started
void im_engine_activate(void *);
// used to ungrab unneeded keys when it's not in selection
bool im_engine_activated(void *);
// maybe called even engined is stopped
void im_engine_deactivate(void *);

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
