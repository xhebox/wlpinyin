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

struct wlpinyin_key {
	uint32_t key;
	uint32_t state;
	struct wl_list link;
};

static int32_t get_miliseconds() {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec * 1000 + time.tv_nsec / (1000 * 1000);
}

static void im_notify(struct wlpinyin_state *state) {
	uint64_t u = 1;
	write(state->im_event_fd, &u, sizeof u);
}

static void im_text_free(struct wlpinyin_state *state) {
	if (state->im_aux_text != NULL) {
		im_engine_aux_free(state->engine, state->im_aux_text);
		state->im_aux_text = NULL;
	}
	for (int i = 0;
			 i < (sizeof state->im_cand_text / sizeof state->im_cand_text[0]); i++) {
		if (state->im_cand_text[i] != NULL) {
			im_engine_candidate_free(state->engine, state->im_cand_text[i]);
			state->im_cand_text[i] = NULL;
		}
	}
}

static void im_send_preedit(struct wlpinyin_state *state, const char *text) {
	zwp_input_method_v2_set_preedit_string(state->input_method, text ? text : "",
																				 0, 0);
	zwp_input_method_v2_commit(state->input_method, state->im_serial++);
}

static void im_send_text(struct wlpinyin_state *state, const char *text) {
	zwp_input_method_v2_commit_string(state->input_method, text ? text : "");
	zwp_input_method_v2_commit(state->input_method, state->im_serial++);
}

static void im_buffer_key(struct wlpinyin_state *state, uint32_t key) {
	if ((state->im_buflen + 2) >= state->im_bufcap) {
		state->im_bufcap += 1024;
		state->im_buf = realloc(state->im_buf, state->im_bufcap);
		if (state->im_buf == NULL) {
			return im_exit(state);
		}
	}

	memmove(&state->im_buf[state->im_bufpos + 1],
					&state->im_buf[state->im_bufpos],
					state->im_buflen - state->im_bufpos);

	state->im_buf[state->im_bufpos++] = key;
	state->im_buf[++state->im_buflen] = 0;
}

static void im_buffer_delete(struct wlpinyin_state *state, int offset) {
	if (offset > 0) {
		if ((state->im_bufpos + offset) > state->im_buflen) {
			offset = state->im_buflen - state->im_bufpos;
		}
		memmove(&state->im_buf[state->im_bufpos],
						&state->im_buf[state->im_bufpos + offset], offset);

		state->im_buflen -= offset;
	} else {
		if ((state->im_bufpos + offset) < 0) {
			offset = -state->im_bufpos;
		}
		memmove(&state->im_buf[state->im_bufpos + offset],
						&state->im_buf[state->im_bufpos], -offset);

		state->im_buflen += offset;
		state->im_bufpos += offset;
	}
	state->im_buf[state->im_buflen] = 0;
}

static void im_buffer_cursor(struct wlpinyin_state *state, int offset) {
	state->im_bufpos += offset;
	if (state->im_bufpos < 0) {
		state->im_bufpos = 0;
	} else if (state->im_bufpos > state->im_buflen) {
		state->im_bufpos = state->im_buflen;
	}
}

static void im_candidate_choose(struct wlpinyin_state *state, int index) {
	size_t offset = im_engine_candidate_choose(state->engine, index);
	memmove(&state->im_buf[0], &state->im_buf[offset], offset);

	state->im_buflen -= offset;
	state->im_bufpos -= offset;
	state->im_buf[state->im_buflen] = 0;
}

static bool im_should_deactivate_engine(struct wlpinyin_state *state) {
	return im_engine_activated(state->engine) && state->im_buflen <= 0;
}

static const char *im_buffer_get(struct wlpinyin_state *state, bool clr) {
	state->im_buf[state->im_buflen] = 0;
	if (clr)
		state->im_buflen = 0;
	return state->im_buf;
}

static void im_auxcand_update(struct wlpinyin_state *state) {
	im_text_free(state);

	im_engine_parse(state->engine, state->im_buf,
									state->im_prefix && (strlen(state->im_prefix) > 0)
											? state->im_prefix
											: "");

	state->im_aux_text = im_engine_aux_get(state->engine, state->im_bufpos);

	for (int i = 0; i < state->im_candidate_page; i++) {
		state->im_cand_text[i] =
				im_engine_candidate_get(state->engine, state->im_candidate_num + i);
	}
}

static void im_panel_update(struct wlpinyin_state *state) {
	char buf[2048] = {};
	int bufptr = 0;

	const char *aux = state->im_aux_text;
	if (!(state->im_aux_text && (strlen(state->im_aux_text) > 0))) {
		aux = im_buffer_get(state, false);
	}

	wlpinyin_dbg("aux[%ld]: %s", strlen(aux), aux);
	bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "%s = ", aux);
	for (int i = 0; i < state->im_candidate_page; i++) {
		wlpinyin_dbg("cand[%d]: %s", i + 1, state->im_cand_text[i]);
		if (state->im_cand_text[i] && (strlen(state->im_cand_text[i]) > 0)) {
			bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "[%d]%s ", i + 1,
												 state->im_cand_text[i]);
		}
	}
	buf[bufptr] = 0;

	im_send_preedit(state, buf);
}

static void im_activate_engine(struct wlpinyin_state *state) {
	if (!im_engine_activated(state->engine)) {
		state->im_aux_text = NULL;
		for (int i = 0;
				 i < (sizeof state->im_cand_text / sizeof state->im_cand_text[0]);
				 i++) {
			state->im_cand_text[i] = NULL;
		}
		state->im_buflen = 0;
		state->im_bufpos = 0;
		state->im_buf[state->im_buflen] = 0;
		state->im_candidate_num = 0;
		im_send_text(state, NULL);
		im_send_preedit(state, NULL);
		im_engine_activate(state->engine);
	}
}

static void im_deactivate_engine(struct wlpinyin_state *state) {
	im_engine_deactivate(state->engine);
	im_send_text(state, NULL);
	im_send_preedit(state, NULL);
	im_text_free(state);
}

static void handle_surrounding(void *data,
															 struct zwp_input_method_v2 *zwp_input_method_v2,
															 const char *text,
															 uint32_t cursor,
															 uint32_t anchor) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	state->im_prefix = text;
}

static void handle_keymap(
		void *data,
		struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2,
		uint32_t format,
		int32_t fd,
		uint32_t size) {
	UNUSED(zwp_input_method_keyboard_grab_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_dbg("keymap %d, %d, %d", format, fd, size);
	char *keymap_string = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (state->xkb_keymap_string == NULL ||
			strcmp(keymap_string, state->xkb_keymap_string) != 0) {
		if (state->xkb_keymap_string != NULL)
			free(state->xkb_keymap_string);
		xkb_state_unref(state->xkb_state);
		xkb_keymap_unref(state->xkb_keymap);

		state->xkb_keymap = xkb_keymap_new_from_string(
				state->xkb_context, keymap_string, XKB_KEYMAP_FORMAT_TEXT_V1,
				XKB_KEYMAP_COMPILE_NO_FLAGS);
		state->xkb_state = xkb_state_new(state->xkb_keymap);
		state->xkb_keymap_string = strdup(keymap_string);
		state->xkb_mods_index[wlpinyin_alt] =
				xkb_keymap_mod_get_index(state->xkb_keymap, XKB_MOD_NAME_ALT);
		state->xkb_mods_index[wlpinyin_ctrl] =
				xkb_keymap_mod_get_index(state->xkb_keymap, XKB_MOD_NAME_CTRL);
		state->xkb_mods_index[wlpinyin_shift] =
				xkb_keymap_mod_get_index(state->xkb_keymap, XKB_MOD_NAME_SHIFT);
		state->xkb_mods_index[wlpinyin_caps] =
				xkb_keymap_mod_get_index(state->xkb_keymap, XKB_MOD_NAME_CAPS);
		state->xkb_mods_index[wlpinyin_win] =
				xkb_keymap_mod_get_index(state->xkb_keymap, XKB_MOD_NAME_LOGO);
		state->xkb_mods_index[wlpinyin_num] =
				xkb_keymap_mod_get_index(state->xkb_keymap, XKB_MOD_NAME_NUM);
		zwp_virtual_keyboard_v1_keymap(state->virtual_keyboard, format, fd, size);
	}
	munmap(keymap_string, size);
	im_notify(state);
}

static void handle_key(
		void *data,
		struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2,
		uint32_t serial,
		uint32_t time,
		uint32_t key,
		uint32_t kstate) {
	UNUSED(zwp_input_method_keyboard_grab_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_dbg("key %d, %d, %d, %d", serial, time, key, kstate);

	struct itimerspec timer = {};
	timerfd_settime(state->im_repeat_timer, 0, &timer, NULL);

	struct wlpinyin_key *keynode = calloc(1, sizeof(struct wlpinyin_key));
	if (state == NULL) {
		wlpinyin_err("failed to calloc key");
		exit(EXIT_FAILURE);
	}

	keynode->key = key;
	keynode->state = kstate;
	wl_list_insert(&state->im_pending_keys, &keynode->link);
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
	wlpinyin_dbg("modifiers %d, %d, %d, %d, %d", serial, mods_depressed,
							 mods_latched, mods_locked, group);
	zwp_virtual_keyboard_v1_modifiers(state->virtual_keyboard, mods_depressed,
																		mods_latched, mods_locked, group);
	state->im_mods = mods_depressed | mods_latched;
	if (state->im_mods != 0)
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
	wlpinyin_dbg("repeat_info %d, %d", rate, delay);
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

	state->im_candidate_num = 0;
	state->im_forwarding = true;

	struct itimerspec timer = {};
	timerfd_settime(state->im_repeat_timer, 0, &timer, NULL);
}

static void im_deactivate(struct wlpinyin_state *state) {
	state->im_forwarding = true;

	struct wlpinyin_key *keynode2 = NULL, *tmp2 = NULL;
	wl_list_for_each_safe(keynode2, tmp2, &state->im_unreleased_keys, link) {
		zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																keynode2->key, WL_KEYBOARD_KEY_STATE_RELEASED);
		wl_list_remove(&keynode2->link);
		free(keynode2);
	}

	struct itimerspec timer = {};
	timerfd_settime(state->im_repeat_timer, 0, &timer, NULL);

	im_deactivate_engine(state);

	if (state->input_method_keyboard_grab != NULL) {
		zwp_input_method_keyboard_grab_v2_release(
				state->input_method_keyboard_grab);
		state->input_method_keyboard_grab = NULL;
	}
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
	im_notify(state);
}

void im_setup(struct wlpinyin_state *state) {
	state->im_bufcap = 1024;

	state->im_buf = malloc(state->im_bufcap);
	if (state->im_buf == NULL) {
		wlpinyin_err("failed to alloc buffer");
		exit(EXIT_FAILURE);
	}

	state->virtual_keyboard =
			zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
					state->virtual_keyboard_manager, state->seat);
	if (state->virtual_keyboard == NULL) {
		wlpinyin_err("failed to setup virtual keyboard");
		exit(EXIT_FAILURE);
	}

	state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (state->xkb_context == NULL) {
		wlpinyin_err("failed to setup xkb context");
		exit(EXIT_FAILURE);
	}

	state->engine = im_engine_new();
	if (state->engine == NULL) {
		wlpinyin_err("failed to setup engine");
		exit(EXIT_FAILURE);
	}

	state->im_event_fd = eventfd(0, EFD_CLOEXEC);
	if (state->im_event_fd == -1) {
		wlpinyin_err("failed to setup event fd");
		exit(EXIT_FAILURE);
	}

	state->im_repeat_timer = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
	if (state->im_repeat_timer == -1) {
		wlpinyin_err("failed to setup repeat timer");
		exit(EXIT_FAILURE);
	}

	wl_list_init(&state->im_pending_keys);
	wl_list_init(&state->im_unreleased_keys);

	state->im_candidate_page = 5 % 10;

	state->input_method = zwp_input_method_manager_v2_get_input_method(
			state->input_method_manager, state->seat);
	if (state->input_method == NULL) {
		wlpinyin_err("failed to setup input_method");
		exit(EXIT_FAILURE);
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
}

int im_event_fd(struct wlpinyin_state *state) {
	return state->im_event_fd;
}

int im_repeat_timerfd(struct wlpinyin_state *state) {
	return state->im_repeat_timer;
}

void im_repeat(struct wlpinyin_state *state, uint64_t times) {
	struct itimerspec timer = {};
	timerfd_gettime(state->im_repeat_timer, &timer);

	if (timer.it_value.tv_nsec == 0 && timer.it_value.tv_sec == 0) {
		return;
	}

	for (uint64_t i = 0; i < times; i++) {
		zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																state->im_repeat_key,
																WL_KEYBOARD_KEY_STATE_PRESSED);
		zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																state->im_repeat_key,
																WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	im_notify(state);
}

void im_exit(struct wlpinyin_state *state) {
	im_deactivate(state);
	state->im_exit = true;
	im_notify(state);
}

bool im_running(struct wlpinyin_state *state) {
	return !(state->im_exit && wl_list_empty(&state->im_unreleased_keys));
}

void im_handle(struct wlpinyin_state *state) {
	struct wlpinyin_key *keynode = NULL, *tmp = NULL;
	wl_list_for_each_safe(keynode, tmp, &state->im_pending_keys, link) {
		xkb_keysym_t keysym =
				xkb_state_key_get_one_sym(state->xkb_state, keynode->key + 8);

		bool handled = false;
		if (!state->im_forwarding &&
				keynode->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
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
					keysym -= XKB_KEY_KP_1;
				case XKB_KEY_1:
				case XKB_KEY_2:
				case XKB_KEY_3:
				case XKB_KEY_4:
				case XKB_KEY_5:
				case XKB_KEY_6:
				case XKB_KEY_7:
				case XKB_KEY_8:
				case XKB_KEY_9:
					if (keysym >= XKB_KEY_1)
						keysym -= XKB_KEY_1;

					if (keysym < state->im_candidate_page) {
						im_candidate_choose(state, state->im_candidate_num + keysym);
						const char *text =
								state->im_cand_text[keysym % (sizeof state->im_cand_text /
																							sizeof state->im_cand_text[0])];
						im_send_text(state, text);
					}
					handled = true;
					break;
				case XKB_KEY_space:
					im_candidate_choose(state, state->im_candidate_num);
					im_send_text(state, state->im_cand_text[0]);
					handled = true;
					break;
				case XKB_KEY_Return:
					im_send_text(state, im_buffer_get(state, true));
					handled = true;
					break;
				case XKB_KEY_Right:
				case XKB_KEY_KP_Right:
					im_buffer_cursor(state, 1);
					handled = true;
					break;
				case XKB_KEY_Left:
				case XKB_KEY_KP_Left:
					im_buffer_cursor(state, -1);
					handled = true;
					break;
				case XKB_KEY_Page_Up:
				case XKB_KEY_KP_Page_Up:
				case XKB_KEY_equal:
				case XKB_KEY_KP_Add:
					state->im_candidate_num += state->im_candidate_page;
					handled = true;
					break;
				case XKB_KEY_Page_Down:
				case XKB_KEY_KP_Page_Down:
				case XKB_KEY_minus:
				case XKB_KEY_KP_Subtract:
					state->im_candidate_num -= state->im_candidate_page;
					handled = true;
					break;
				case XKB_KEY_Delete:
					im_buffer_delete(state, 1);
					handled = true;
					break;
				case XKB_KEY_BackSpace:
					im_buffer_delete(state, -1);
					handled = true;
					break;
				}
			}

			switch (keysym) {
			case XKB_KEY_c:
			case XKB_KEY_z:
			case XKB_KEY_C:
			case XKB_KEY_Z:
				if (state->im_mods & (1 << state->xkb_mods_index[wlpinyin_ctrl])) {
					im_engine_deactivate(state->engine);
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
				im_buffer_key(state, xkb_keysym_to_utf32(keysym));
				handled = true;
				break;
			}
		}

		if (im_should_deactivate_engine(state)) {
			im_deactivate_engine(state);
		}

		if (im_engine_activated(state->engine)) {
			im_auxcand_update(state);
			im_panel_update(state);
		}

		wl_list_remove(&keynode->link);

		if (keynode->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
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

			if (!handled) {
				zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																		keynode->key, keynode->state);

				if (xkb_key_repeats(state->xkb_keymap, keynode->key) == 1) {
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
					state->im_repeat_key = keynode->key;
				}

				wl_list_insert(&state->im_unreleased_keys, &keynode->link);
			}
		} else if (keynode->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
			struct wlpinyin_key *keynode2 = NULL, *tmp2 = NULL;
			wl_list_for_each_safe(keynode2, tmp2, &state->im_unreleased_keys, link) {
				if (keynode2->key == keynode->key) {
					zwp_virtual_keyboard_v1_key(state->virtual_keyboard,
																			get_miliseconds(), keynode->key,
																			keynode->state);
					wl_list_remove(&keynode2->link);
					free(keynode2);
					break;
				}
			}

			if (state->im_only_modifier && keysym == XKB_KEY_Control_L) {
				if (state->im_forwarding) {
					state->im_forwarding = false;
				} else {
					state->im_forwarding = true;
					im_deactivate_engine(state);
				}
			}

			free(keynode);
		}
	}

	wl_display_roundtrip(state->display);
}

void im_destroy(struct wlpinyin_state *state) {
	if (state->im_buf != NULL) {
		free(state->im_buf);
	}

	im_deactivate(state);
	if (state->xkb_keymap_string) {
		free(state->xkb_keymap_string);
		state->xkb_keymap_string = NULL;
	}
	im_engine_free(state->engine);
	xkb_state_unref(state->xkb_state);
	xkb_keymap_unref(state->xkb_keymap);
	xkb_context_unref(state->xkb_context);
	zwp_virtual_keyboard_v1_destroy(state->virtual_keyboard);
	zwp_input_method_v2_destroy(state->input_method);
}
