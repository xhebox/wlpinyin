#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "wlpinyin.h"

#define MIN(a, b) (a < b ? a : b)

struct wlpinyin_key {
	xkb_keysym_t keysym;
	uint32_t keycode;
	bool pressed;
};

static int32_t get_miliseconds() {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec * 1000 + time.tv_nsec / (1000 * 1000);
}

static void im_send_preedit(struct wlpinyin_state *state, const char *text) {
	wlpinyin_dbg("send_preedit: %s", text ? text : "");
	zwp_input_method_v2_set_preedit_string(state->input_method, text ? text : "",
																				 0, 0);
}

static void im_send_text(struct wlpinyin_state *state, const char *text) {
	wlpinyin_dbg("send_text: %s", text ? text : "");
	zwp_input_method_v2_commit_string(state->input_method, text ? text : "");
}

static void noop() {}

static void im_panel_update(struct wlpinyin_state *state) {
	char buf[512] = {};
	int bufptr = 0;

	int im_candidate_len = im_engine_candidate_len(state->engine);

	const char *preedit = im_engine_preedit_get(state->engine);
	if (strlen(preedit) == 0) {
		im_send_preedit(state, "");
		return;
	}

	wlpinyin_dbg("preedit: %s", preedit);
	bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "%s <- ", preedit);

	for (int i = 0; i < im_candidate_len; i++) {
		const char *cand = im_engine_candidate_get(state->engine, i);
		wlpinyin_dbg("cand[%d]: %s", i + 1, cand);
		bufptr +=
				snprintf(&buf[bufptr], sizeof buf - bufptr, " [%d]%s", i + 1, cand);
	}
	buf[bufptr] = 0;

	im_send_preedit(state, buf);
}

static void im_handle_key(struct wlpinyin_state *state,
													struct wlpinyin_key *keynode) {
	if (state->xkb_state == NULL)
		return;

	xkb_state_update_key(state->xkb_state, keynode->keycode + 8,
											 keynode->pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

	bool handled = false;

	bool has_modifiers =
			xkb_state_mod_names_are_active(
					state->xkb_state, XKB_STATE_MODS_EFFECTIVE, XKB_STATE_MATCH_ANY,
					XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CAPS, XKB_MOD_NAME_CTRL,
					XKB_MOD_NAME_ALT, XKB_MOD_NAME_NUM, XKB_MOD_NAME_LOGO, NULL) == 1;
	if (keynode->pressed) {
		if (state->im_activated) {
			if (!has_modifiers && strlen(im_engine_preedit_get(state->engine)) != 0) {
				xkb_keysym_t keysym = keynode->keysym;
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
					im_engine_candidate_choose(state->engine, keysym);
					handled = true;
					break;
				case XKB_KEY_1:
				case XKB_KEY_2:
				case XKB_KEY_3:
				case XKB_KEY_4:
				case XKB_KEY_5:
				case XKB_KEY_6:
				case XKB_KEY_7:
				case XKB_KEY_8:
				case XKB_KEY_9:
					keysym -= XKB_KEY_1;
					im_engine_candidate_choose(state->engine, keysym);
					handled = true;
					break;
				case XKB_KEY_space:
					im_engine_candidate_choose(state->engine, 0);
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
				case XKB_KEY_UP:
				case XKB_KEY_KP_Up:
				case XKB_KEY_Page_Up:
				case XKB_KEY_KP_Page_Up:
				case XKB_KEY_equal:
				case XKB_KEY_KP_Add:
					im_engine_page(state->engine, true);
					handled = true;
					break;
				case XKB_KEY_DOWN:
				case XKB_KEY_KP_Down:
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
				handled = im_engine_key(
						state->engine, keynode->keysym,
						xkb_state_serialize_mods(
								state->xkb_state,
								XKB_STATE_MODS_EFFECTIVE | XKB_STATE_LAYOUT_EFFECTIVE));
			}
		}
	}

	if (!handled) {
		if (im_toggle(state->xkb_state, keynode->keysym, keynode->pressed)) {
			wlpinyin_dbg("toggle");
			state->im_activated = !state->im_activated;
			im_engine_reset(state->engine);
			handled = true;
		}
	}

	if (handled) {
		im_panel_update(state);
		const char *commit = im_engine_commit_text(state->engine);
		if (strlen(commit) != 0)
			im_send_text(state, commit);
		zwp_input_method_v2_commit(state->input_method, state->im_serial++);
	} else {
		wlpinyin_dbg("send_key: keycode %d, pressed %d", keynode->keycode,
								 keynode->pressed);
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
	wl_display_flush(state->display);
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

	char *keymap_string = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (state->xkb_keymap_string != NULL &&
			!strcmp(keymap_string, state->xkb_keymap_string)) {
		munmap(keymap_string, size);
		return;
	}

	xkb_state_unref(state->xkb_state);
	xkb_keymap_unref(state->xkb_keymap);
	if (state->xkb_keymap_string != NULL)
		free(state->xkb_keymap_string);

	state->xkb_keymap = xkb_keymap_new_from_string(
			state->xkb_context, keymap_string, format, XKB_KEYMAP_COMPILE_NO_FLAGS);
	state->xkb_keymap_string = strdup(keymap_string);
	state->xkb_keymap =
			xkb_keymap_new_from_string(state->xkb_context, state->xkb_keymap_string,
																 format, XKB_KEYMAP_COMPILE_NO_FLAGS);
	state->xkb_state = xkb_state_new(state->xkb_keymap);

	munmap(keymap_string, size);
	zwp_virtual_keyboard_v1_keymap(state->virtual_keyboard, format, fd, size);
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

	struct wlpinyin_key keynode = {};
	keynode.keycode = key;
	keynode.keysym = xkb_state_key_get_one_sym(state->xkb_state, key + 8);
	keynode.pressed = kstate == WL_KEYBOARD_KEY_STATE_PRESSED;

#ifndef NDEBUG
	char buf[512] = {};
	xkb_keysym_get_name(keynode.keysym, buf, sizeof buf);
	wlpinyin_dbg("key[%s]: serial %d, time %d, %s", buf, serial, time,
							 keynode.pressed ? "pressed" : "released");
#endif

	// reset repeat key
	if (keynode.pressed &&
			xkb_keymap_key_repeats(state->xkb_keymap, keynode.keycode + 8) == 1) {
		state->im_repeat_key = keynode.keycode;
		struct itimerspec timer = {
				.it_value = {.tv_nsec = state->im_repeat_delay * 1000000},
				.it_interval = {.tv_nsec = 1000000000 / state->im_repeat_rate},
		};
		timerfd_settime(state->timerfd, 0, &timer, NULL);
	} else {
		struct itimerspec timer = {};
		timerfd_settime(state->timerfd, 0, &timer, NULL);
	}

	// handle it
	im_handle_key(state, &keynode);
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
	xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
												mods_locked, 0, 0, group);
	zwp_virtual_keyboard_v1_modifiers(state->virtual_keyboard, mods_depressed,
																		mods_latched, mods_locked, group);
}

static void handle_repeat_info(
		void *data,
		struct zwp_input_method_keyboard_grab_v2 *zwp_input_method_keyboard_grab_v2,
		int32_t rate,
		int32_t delay) {
	UNUSED(zwp_input_method_keyboard_grab_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_dbg("repeat_info: rate %d, delay %d", rate, delay);
	state->im_repeat_delay = (uint32_t)delay;
	state->im_repeat_rate = (uint32_t)rate;
}

static void handle_reset(void *data,
												 struct zwp_input_method_v2 *zwp_input_method_v2) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_dbg("reset");
	im_engine_reset(state->engine);
}

static void handle_global(void *data,
													struct wl_registry *registry,
													uint32_t name,
													const char *interface,
													uint32_t version) {
	struct wlpinyin_state *state = data;
	if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
		state->input_method_manager = wl_registry_bind(
				registry, name, &zwp_input_method_manager_v2_interface, 1);
	} else if (strcmp(interface,
										zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
		state->virtual_keyboard_manager = wl_registry_bind(
				registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->seat = wl_registry_bind(registry, name, &wl_seat_interface, version);
	} else if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor =
				wl_registry_bind(registry, name, &wl_compositor_interface, version);
	}
}

struct wlpinyin_state *im_setup(int signalfd, struct wl_display *display) {
	struct wlpinyin_state *state = calloc(1, sizeof(struct wlpinyin_state));
	if (state == NULL) {
		wlpinyin_err("failed to calloc state");
		return NULL;
	}
	state->signalfd = signalfd;
	state->display = display;
	state->im_activated = default_activation;

	{
		struct wl_registry *registry = wl_display_get_registry(state->display);
		static const struct wl_registry_listener registry_listener = {
				.global = handle_global,
				.global_remove = NULL,
		};
		wl_registry_add_listener(registry, &registry_listener, state);
		wl_display_roundtrip(state->display);

		if (state->input_method_manager == NULL ||
				state->virtual_keyboard_manager == NULL || state->seat == NULL ||
				state->compositor == NULL) {
			wlpinyin_err("required wayland interface not available");
			goto clean;
		}
	}

	state->timerfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
	if (state->timerfd == -1) {
		wlpinyin_err("failed to setup event timer");
		goto clean;
	}

	state->im_serial = 1;

	state->engine = im_engine_new();
	if (state->engine == NULL) {
		wlpinyin_err("failed to setup engine");
		goto clean;
	}

	state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (state->xkb_context == NULL) {
		wlpinyin_err("failed to setup xkb context");
		goto clean;
	}

	state->virtual_keyboard =
			zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
					state->virtual_keyboard_manager, state->seat);
	if (state->virtual_keyboard == NULL) {
		wlpinyin_err("failed to setup virtual keyboard");
		goto clean;
	}

	state->input_method = zwp_input_method_manager_v2_get_input_method(
			state->input_method_manager, state->seat);
	if (state->input_method == NULL) {
		wlpinyin_err("failed to setup input_method");
		goto clean;
	}

	state->popup_surface_wl = wl_compositor_create_surface(state->compositor);
	if (state->popup_surface_wl == NULL) {
		wlpinyin_err("failed to create surface");
		goto clean;
	}

	state->popup_surface = zwp_input_method_v2_get_input_popup_surface(
			state->input_method, state->popup_surface_wl);
	if (state->popup_surface_wl == NULL) {
		wlpinyin_err("failed to get popup surface");
		goto clean;
	}

	static const struct zwp_input_method_v2_listener im_listener = {
			.activate = handle_reset,
			.deactivate = handle_reset,
			.surrounding_text = noop,
			.text_change_cause = noop,
			.content_type = noop,
			.done = noop,
			.unavailable = handle_reset,
	};
	zwp_input_method_v2_add_listener(state->input_method, &im_listener, state);

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

	wl_display_roundtrip(state->display);
	return state;

clean:
	im_destroy(state);
	return NULL;
}

int im_loop(struct wlpinyin_state *state) {
	enum {
		fd_signal = 0,
		fd_wayland,
		fd_repeat,
		fd_max,
	};

	struct pollfd fds[fd_max] = {};

	fds[fd_signal].fd = state->signalfd;
	fds[fd_signal].events = POLLIN;

	fds[fd_wayland].fd = wl_display_get_fd(state->display);
	fds[fd_wayland].events = POLLIN;

	fds[fd_repeat].fd = state->timerfd;
	fds[fd_repeat].events = POLLIN;

	bool running = true;

	while (running && poll(fds, sizeof fds / sizeof fds[fd_wayland], -1) != -1) {
		if (fds[fd_signal].revents & POLLIN) {
			fds[fd_signal].revents = 0;
			struct signalfd_siginfo info = {};
			read(fds[fd_signal].fd, &info, sizeof(info));
			switch (info.ssi_signo) {
			case SIGINT:
			case SIGTERM:
				running = false;
				break;
			}
			wlpinyin_dbg("signal: %d, running: %d", info.ssi_signo, running);
		} else if (fds[fd_wayland].revents & POLLIN) {
			fds[fd_wayland].revents = 0;
			if (wl_display_roundtrip(state->display) == -1) {
				break;
			}
		} else if (fds[fd_repeat].revents & POLLIN) {
			fds[fd_repeat].revents = 0;
			uint64_t tick;
			read(fds[fd_repeat].fd, &tick, sizeof tick);
			struct wlpinyin_key keynode = {
					.keycode = state->im_repeat_key,
					.keysym = xkb_state_key_get_one_sym(state->xkb_state,
																							state->im_repeat_key + 8),
			};
			for (; tick != 0; tick--) {
				keynode.pressed = true;
				im_handle_key(state, &keynode);
				keynode.pressed = false;
				im_handle_key(state, &keynode);
			}
		}
	}

	return 0;
}

int im_destroy(struct wlpinyin_state *state) {
	if (state->input_method_keyboard_grab != NULL)
		zwp_input_method_keyboard_grab_v2_release(
				state->input_method_keyboard_grab);
	if (state->engine)
		im_engine_free(state->engine);
	if (state->popup_surface)
		zwp_input_popup_surface_v2_destroy(state->popup_surface);
	if (state->popup_surface_wl)
		wl_surface_destroy(state->popup_surface_wl);
	if (state->virtual_keyboard)
		zwp_virtual_keyboard_v1_destroy(state->virtual_keyboard);
	if (state->input_method)
		zwp_input_method_v2_destroy(state->input_method);
	if (state->xkb_keymap_string)
		free(state->xkb_keymap_string);
	if (state->xkb_state)
		xkb_state_unref(state->xkb_state);
	if (state->xkb_keymap)
		xkb_keymap_unref(state->xkb_keymap);
	if (state->xkb_context)
		xkb_context_unref(state->xkb_context);

	wl_display_flush(state->display);
	return 0;
}
