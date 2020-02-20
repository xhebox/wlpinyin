#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "wlpinyin.h"

static int32_t get_miliseconds() {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec * 1000 + time.tv_nsec / (1000 * 1000);
}

static void im_notify(struct wlpinyin_state *state) {
	uint64_t u = 1;
	write(state->im_event_fd, &u, sizeof u);
}

static void im_send_preedit(struct wlpinyin_state *state, const char *text) {
	zwp_input_method_v2_set_preedit_string(state->input_method, text ? text : "",
																				 0, 0);
	zwp_input_method_v2_commit(state->input_method, state->im_serial);
}

static void im_send_text(struct wlpinyin_state *state, const char *text) {
	zwp_input_method_v2_commit_string(state->input_method, text ? text : "");
	zwp_input_method_v2_commit(state->input_method, state->im_serial);
}

static void im_panel_update(struct wlpinyin_state *state) {
	char buf[512] = {};
	int bufptr = 0;

	state->im_candidate_len = im_engine_candidate_len(state->engine);

	const char *preedit = im_engine_preedit_get(state->engine);
	if (preedit == NULL) {
		im_send_preedit(state, "");
		return;
	}

	wlpinyin_dbg("preedit: %s", preedit);
	bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "%s <- ", preedit);

	for (int i = 0; i < state->im_candidate_len; i++) {
		const char *cand = im_engine_candidate_get(state->engine, i);
		wlpinyin_dbg("cand[%d]: %s", i + 1, cand);
		bufptr +=
				snprintf(&buf[bufptr], sizeof buf - bufptr, " [%d]%s", i + 1, cand);
	}
	buf[bufptr] = 0;

	im_send_preedit(state, buf);
}

static void im_choose_candidate(struct wlpinyin_state *state, int index) {
	if (index >= state->im_candidate_len)
		return;

	im_engine_candidate_choose(state->engine, index);
	const char *text = im_engine_commit_text(state->engine);
	if (text != NULL) {
		im_send_text(state, text);
	}
}

static void im_activate_engine(struct wlpinyin_state *state) {
	if (!im_engine_activated(state->engine)) {
		im_engine_activate(state->engine);
	}
}

static void im_deactivate_engine(struct wlpinyin_state *state) {
	if (im_engine_activated(state->engine)) {
		im_send_preedit(state, "");
		im_engine_deactivate(state->engine);
	}
}

static void handle_surrounding(void *data,
															 struct zwp_input_method_v2 *zwp_input_method_v2,
															 const char *text,
															 uint32_t cursor,
															 uint32_t anchor) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	if (state->im_prefix != NULL)
		free((void *)state->im_prefix);

	state->im_prefix = strdup(text);
	state->im_prefix[cursor] = 0;
	// wlpinyin_dbg("surround: %d,%d, %s", cursor, anchor, text);
}

static void handle_keymap(
		void *data,
		struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2,
		uint32_t format,
		int32_t fd,
		uint32_t size) {
	UNUSED(zwp_input_method_keyboard_grab_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_dbg("keymap: format %d, size %d, fd %d", format, size, fd);

	xkb_state_unref(state->xkb_state);
	xkb_keymap_unref(state->xkb_keymap);

	char *keymap_string = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	state->xkb_keymap = xkb_keymap_new_from_string(
			state->xkb_context, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(keymap_string, size);

	state->xkb_state = xkb_state_new(state->xkb_keymap);
	zwp_virtual_keyboard_v1_keymap(state->virtual_keyboard, format, fd, size);
	im_notify(state);
}

static void im_add_pressed(struct wlpinyin_state *state, uint32_t keycode) {
	if (state->im_pressed_num + 1 >= state->im_pressed_cap) {
		state->im_pressed_cap += 64;
		state->im_pressed =
				malloc(sizeof(state->im_pressed[0]) * state->im_pressed_cap);
		if (state->im_pressed == NULL) {
			im_exit(state);
			return;
		}
	}

	state->im_pressed[state->im_pressed_num++] = keycode;
}

static bool im_del_pressed(struct wlpinyin_state *state, uint32_t keycode) {
	bool r = false;
	for (int i = 0; i < state->im_pressed_num; i++) {
		if (state->im_pressed[i] == keycode) {
			int last = --state->im_pressed_num;
			state->im_pressed[i] = state->im_pressed[last];
			state->im_pressed[last] = 0;
			r = true;
			break;
		}
	}
	return r;
}

static bool im_handle_key(struct wlpinyin_state *state,
													struct wlpinyin_key *keynode);
static void handle_key(
		void *data,
		struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2,
		uint32_t serial,
		uint32_t time,
		uint32_t key,
		uint32_t kstate) {
	UNUSED(zwp_input_method_keyboard_grab_v2);
	struct wlpinyin_state *state = data;
	xkb_keysym_t keysym = xkb_state_key_get_one_sym(state->xkb_state, key + 8);

	struct itimerspec timer = {};
	timerfd_settime(state->im_repeat_timer, 0, &timer, NULL);

	struct wlpinyin_key keynode = {};
	keynode.keycode = key;
	keynode.keysym = keysym;
	keynode.pressed = kstate == WL_KEYBOARD_KEY_STATE_PRESSED;

	xkb_state_update_key(state->xkb_state, key + 8,
											 keynode.pressed ? XKB_KEY_UP : XKB_KEY_DOWN);

#ifndef NDEBUG
	char buf[512] = {};
	xkb_keysym_get_name(keysym, buf, sizeof buf);
	wlpinyin_dbg("key[%s]: serial %d, time %d, %s", buf, serial, time,
							 keynode.pressed ? "pressed" : "released");
#endif

	if (keynode.pressed) {
		im_add_pressed(state, keynode.keycode);
		struct itimerspec timer = {
				.it_value =
						{
								.tv_nsec = state->im_repeat_delay * 1000000,
								.tv_sec = 0,
						},
				.it_interval =
						{
								.tv_nsec = 1000000000 / state->im_repeat_rate,
								.tv_sec = 0,
						},
		};
		timerfd_settime(state->im_repeat_timer, 0, &timer, NULL);
		state->im_repeat_key = keynode.keycode;

		im_handle_key(state, &keynode);
	} else {
		if (im_del_pressed(state, keynode.keycode)) {
			im_handle_key(state, &keynode);
		}
	}

	im_notify(state);
}

static void handle_modifiers(
		void *data,
		struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2,
		uint32_t serial,
		uint32_t mods_depressed,
		uint32_t mods_latched,
		uint32_t mods_locked,
		uint32_t group) {
	UNUSED(zwp_input_method_keyboard_grab_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_dbg(
			"modifiers: serial %d, depressed %d, latched %d, locked %d, group %d",
			serial, mods_depressed, mods_latched, mods_locked, group);
	zwp_virtual_keyboard_v1_modifiers(state->virtual_keyboard, mods_depressed,
																		mods_latched, mods_locked, group);
	xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
												mods_locked, 0, 0, group);
	state->im_only_modifier = true;
	im_notify(state);
}

static void handle_repeat_info(
		void *data,
		struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2,
		int32_t rate,
		int32_t delay) {
	UNUSED(zwp_input_method_keyboard_grab_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_dbg("repeat_info: rate %d, delay %d", rate, delay);
	state->im_repeat_delay = delay;
	state->im_repeat_rate = rate;
	im_notify(state);
}

static void im_activate(struct wlpinyin_state *state) {
	if (state->input_method_keyboard_grab == NULL) {
		state->input_method_keyboard_grab =
				zwp_input_method_v2_grab_keyboard(state->input_method);
		static const struct zwp_input_method_keyboard_grab_v2_listener
				im_activate_listener = {
						.keymap = handle_keymap,
						.key = handle_key,
						.modifiers = handle_modifiers,
						.repeat_info = handle_repeat_info,
				};
		zwp_input_method_keyboard_grab_v2_add_listener(
				state->input_method_keyboard_grab, &im_activate_listener, state);
	}

	state->im_forwarding = true;

	struct itimerspec timer = {};
	timerfd_settime(state->im_repeat_timer, 0, &timer, NULL);

	wl_display_roundtrip(state->display);
}

static void im_deactivate(struct wlpinyin_state *state) {
	state->im_forwarding = true;

	for (int i = 0; i < state->im_pressed_num; i++) {
		zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																state->im_pressed[i],
																WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	state->im_pressed_num = 0;

	struct itimerspec timer = {};
	timerfd_settime(state->im_repeat_timer, 0, &timer, NULL);

	im_deactivate_engine(state);

	if (state->input_method_keyboard_grab != NULL) {
		zwp_input_method_keyboard_grab_v2_release(
				state->input_method_keyboard_grab);
		state->input_method_keyboard_grab = NULL;
	}
	wl_display_roundtrip(state->display);
}

static void handle_activate(void *data,
														struct zwp_input_method_v2 *zwp_input_method_v2) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_err("activate");
	im_activate(state);
}

static void handle_deactivate(void *data,
															struct zwp_input_method_v2 *zwp_input_method_v2) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_err("deactivate");
	im_deactivate(state);
}

static void handle_unavailable(
		void *data,
		struct zwp_input_method_v2 *zwp_input_method_v2) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	im_exit(state);
}

static void handle_done(void *data,
												struct zwp_input_method_v2 *zwp_input_method_v2) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	state->im_serial++;
	im_notify(state);
}

int im_setup(struct wlpinyin_state *state) {
	state->im_serial = 1;

	state->virtual_keyboard =
			zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
					state->virtual_keyboard_manager, state->seat);
	if (state->virtual_keyboard == NULL) {
		wlpinyin_err("failed to setup virtual keyboard");
		return -1;
	}

	state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (state->xkb_context == NULL) {
		wlpinyin_err("failed to setup xkb context");
		return -1;
	}

	state->engine = im_engine_new();
	if (state->engine == NULL) {
		wlpinyin_err("failed to setup engine");
		return -1;
	}

	state->im_event_fd = eventfd(0, EFD_CLOEXEC);
	if (state->im_event_fd == -1) {
		wlpinyin_err("failed to setup event fd");
		return -1;
	}

	state->im_repeat_timer = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
	if (state->im_repeat_timer == -1) {
		wlpinyin_err("failed to setup repeat timer");
		return -1;
	}

	state->im_pressed_cap = 64;
	state->im_pressed_num = 0;
	state->im_pressed =
			malloc(sizeof(state->im_pressed[0]) * state->im_pressed_cap);
	if (state->im_pressed == NULL) {
		wlpinyin_err("failed to setup unreleased keys");
		return -1;
	}

	state->input_method = zwp_input_method_manager_v2_get_input_method(
			state->input_method_manager, state->seat);
	if (state->input_method == NULL) {
		wlpinyin_err("failed to setup input_method");
		return -1;
	}

	static const struct zwp_input_method_v2_listener im_listener = {
			.activate = handle_activate,
			.deactivate = handle_deactivate,
			.surrounding_text = handle_surrounding,
			.text_change_cause = noop,
			.content_type = noop,
			.done = handle_done,
			.unavailable = handle_unavailable,
	};
	zwp_input_method_v2_add_listener(state->input_method, &im_listener, state);

	wl_display_roundtrip(state->display);
	return 0;
}

int im_event_fd(struct wlpinyin_state *state) {
	return state->im_event_fd;
}

int im_repeat_timerfd(struct wlpinyin_state *state) {
	return state->im_repeat_timer;
}

void im_repeat(struct wlpinyin_state *state, uint64_t times) {
	state->im_repeat_times += times;
	im_notify(state);
}

void im_exit(struct wlpinyin_state *state) {
	im_deactivate(state);
	state->im_exit = true;
	im_notify(state);
}

bool im_running(struct wlpinyin_state *state) {
	return !(state->im_exit && state->im_pressed_num == 0);
}

static bool im_handle_key(struct wlpinyin_state *state,
													struct wlpinyin_key *keynode) {
	xkb_keysym_t keysym = keynode->keysym;
	bool pressed = keynode->pressed;

	bool handled = false;

	if (!state->im_forwarding && pressed) {
		if (im_engine_activated(state->engine)) {
			switch (keysym) {
			case XKB_KEY_KP_1:
			case XKB_KEY_KP_2:
			case XKB_KEY_KP_3:
			case XKB_KEY_KP_4:
			case XKB_KEY_KP_5:
			case XKB_KEY_KP_6:
			case XKB_KEY_KP_7:
			case XKB_KEY_KP_8:
			case XKB_KEY_KP_9:
				if ((keysym - XKB_KEY_KP_1) < state->im_candidate_len) {
					keysym -= XKB_KEY_KP_1;
				}
			case XKB_KEY_1:
			case XKB_KEY_2:
			case XKB_KEY_3:
			case XKB_KEY_4:
			case XKB_KEY_5:
			case XKB_KEY_6:
			case XKB_KEY_7:
			case XKB_KEY_8:
			case XKB_KEY_9:
				if ((keysym - XKB_KEY_1) < state->im_candidate_len) {
					keysym -= XKB_KEY_1;
				}

				if (keysym < 9) {
					im_choose_candidate(state, keysym);
				}
				handled = true;
				break;
			case XKB_KEY_space:
				im_choose_candidate(state, 0);
				handled = true;
				break;
			case XKB_KEY_Return:
				im_send_text(state, im_engine_preedit_get(state->engine));
			case XKB_KEY_Escape:
				im_deactivate_engine(state);
				handled = true;
				break;
			case XKB_KEY_Right:
			case XKB_KEY_KP_Right:
				im_engine_cursor(state->engine, true);
				handled = true;
				break;
			case XKB_KEY_Left:
			case XKB_KEY_KP_Left:
				im_engine_cursor(state->engine, false);
				handled = true;
				break;
			case XKB_KEY_Page_Up:
			case XKB_KEY_KP_Page_Up:
			case XKB_KEY_equal:
			case XKB_KEY_KP_Add:
				im_engine_page(state->engine, true);
				handled = true;
				break;
			case XKB_KEY_Page_Down:
			case XKB_KEY_KP_Page_Down:
			case XKB_KEY_minus:
			case XKB_KEY_KP_Subtract:
				im_engine_page(state->engine, false);
				handled = true;
				break;
			case XKB_KEY_Delete:
				im_engine_delete(state->engine, true);
				handled = true;
				break;
			case XKB_KEY_BackSpace:
				im_engine_delete(state->engine, false);
				handled = true;
				break;
			}
		}

		if (!handled) {
			switch (keysym) {
			case XKB_KEY_c:
			case XKB_KEY_z:
			case XKB_KEY_C:
			case XKB_KEY_Z:
				if (xkb_state_mod_name_is_active(state->xkb_state, XKB_MOD_NAME_CTRL,
																				 XKB_STATE_MODS_DEPRESSED) > 0) {
					im_deactivate_engine(state);
					im_exit(state);
					handled = true;
					break;
				}
			case XKB_KEY_a:
			case XKB_KEY_b:
			case XKB_KEY_d:
			case XKB_KEY_e:
			case XKB_KEY_f:
			case XKB_KEY_g:
			case XKB_KEY_h:
			case XKB_KEY_i:
			case XKB_KEY_j:
			case XKB_KEY_k:
			case XKB_KEY_l:
			case XKB_KEY_m:
			case XKB_KEY_n:
			case XKB_KEY_o:
			case XKB_KEY_p:
			case XKB_KEY_q:
			case XKB_KEY_r:
			case XKB_KEY_s:
			case XKB_KEY_t:
			case XKB_KEY_u:
			case XKB_KEY_v:
			case XKB_KEY_w:
			case XKB_KEY_x:
			case XKB_KEY_y:
			case XKB_KEY_A:
			case XKB_KEY_B:
			case XKB_KEY_D:
			case XKB_KEY_E:
			case XKB_KEY_F:
			case XKB_KEY_G:
			case XKB_KEY_H:
			case XKB_KEY_I:
			case XKB_KEY_J:
			case XKB_KEY_K:
			case XKB_KEY_L:
			case XKB_KEY_M:
			case XKB_KEY_N:
			case XKB_KEY_O:
			case XKB_KEY_P:
			case XKB_KEY_Q:
			case XKB_KEY_R:
			case XKB_KEY_S:
			case XKB_KEY_T:
			case XKB_KEY_U:
			case XKB_KEY_V:
			case XKB_KEY_W:
			case XKB_KEY_X:
			case XKB_KEY_Y:
				im_activate_engine(state);
				im_engine_key(state->engine, keysym,
											xkb_state_serialize_mods(state->xkb_state,
																							 XKB_STATE_MODS_DEPRESSED) |
													xkb_state_serialize_mods(state->xkb_state,
																									 XKB_STATE_MODS_LATCHED));
				handled = true;
				break;
			}
		}

		if (handled) {
			const char *text = im_engine_commit_text(state->engine);
			if (text != NULL) {
				im_deactivate_engine(state);
			}

			if (im_engine_activated(state->engine)) {
				im_panel_update(state);
			}
		}
	}

	if (pressed) {
		switch (keysym) {
		case XKB_KEY_Control_L:
		case XKB_KEY_Control_R:
		case XKB_KEY_Shift_L:
		case XKB_KEY_Shift_R:
		case XKB_KEY_Alt_L:
		case XKB_KEY_Alt_R:
		case XKB_KEY_Meta_L:
		case XKB_KEY_Meta_R:
		case XKB_KEY_Hyper_L:
		case XKB_KEY_Hyper_R:
		case XKB_KEY_Shift_Lock:
		case XKB_KEY_Caps_Lock:
			break;
		default:
			state->im_only_modifier = false;
		}
	} else {
		if (im_toggle(state->im_only_modifier, state->xkb_state, keysym)) {
			if (state->im_forwarding) {
				state->im_forwarding = false;
			} else {
				state->im_forwarding = true;
				im_deactivate_engine(state);
			}
		}
	}

	if (!handled) {
		if (keynode->pressed) {
			zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																	keynode->keycode,
																	WL_KEYBOARD_KEY_STATE_PRESSED);
		} else {
			zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																	keynode->keycode,
																	WL_KEYBOARD_KEY_STATE_RELEASED);
		}
		handled = true;
	}

	return handled;
}

void im_handle(struct wlpinyin_state *state) {
	struct wlpinyin_key _keynode, *keynode = &_keynode;
	if (state->xkb_state != NULL) {
		keynode->keycode = state->im_repeat_key;
		keynode->keysym =
				xkb_state_key_get_one_sym(state->xkb_state, keynode->keycode + 8);

		for (uint64_t i = 0; i < state->im_repeat_times; i++) {
			if (state->im_forwarding) {
				zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																		state->im_repeat_key,
																		WL_KEYBOARD_KEY_STATE_PRESSED);
				zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																		state->im_repeat_key,
																		WL_KEYBOARD_KEY_STATE_RELEASED);
			} else {
				keynode->pressed = true;
				im_handle_key(state, keynode);

				keynode->pressed = false;
				im_handle_key(state, keynode);
			}
		}
		state->im_repeat_times = 0;
	}

	wl_display_roundtrip(state->display);
}

void im_destroy(struct wlpinyin_state *state) {
	if (state->im_prefix != NULL)
		free(state->im_prefix);

	if (state->im_pressed != NULL)
		free(state->im_pressed);
	im_deactivate(state);
	im_engine_free(state->engine);
	xkb_state_unref(state->xkb_state);
	xkb_keymap_unref(state->xkb_keymap);
	xkb_context_unref(state->xkb_context);
	zwp_virtual_keyboard_v1_destroy(state->virtual_keyboard);
	zwp_input_method_v2_destroy(state->input_method);
}
