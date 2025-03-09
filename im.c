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
#include <wayland-client-protocol.h>

#include "input-method-unstable-v2-client-protocol.h"
#include "wlpinyin.h"

#define MIN(a, b) (a < b ? a : b)

struct wlpinyin_key {
	xkb_keysym_t xkb_keysym;
	xkb_keycode_t xkb_keycode;
	uint32_t keycode;
	bool pressed;
};

static int32_t get_miliseconds() {
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec * 1000 + time.tv_nsec / (1000 * 1000);
}

static void im_send_preedit(struct wlpinyin_state *state, const char *text) {
	wlpinyin_dbg("upd_preedit: %s", text ? text : "");
	zwp_input_method_v2_set_preedit_string(state->input_method, text ? text : "",
																				 0, 0);
}

static void im_send_text(struct wlpinyin_state *state, const char *text) {
	wlpinyin_dbg("upd_text: %s", text ? text : "");
	zwp_input_method_v2_commit_string(state->input_method, text ? text : "");
}

static void noop() {}

static void im_panel_update(struct wlpinyin_state *state) {
	char buf[2048] = {};
	int bufptr = 0;

	preedit_t preedit = im_engine_preedit(state->engine);
	if (!preedit.text) {
		im_send_preedit(state, "");
		return;
	}

	candidate_t cand = im_engine_candidate(state->engine);
	bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "[%d] ", cand.page_no);

	int preedit_len = strlen(preedit.text);
	for (int i = 0; i <= preedit_len; i++) {
		if (preedit.start < preedit.end) {
			if (i == preedit.start)
				bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "<");
			else if (i == preedit.end)
				bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, ">");
		}
		if (i == preedit.cursor)
			bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, "|");
		if (i < preedit_len)
			bufptr +=
					snprintf(&buf[bufptr], sizeof buf - bufptr, "%c", preedit.text[i]);
	}

	for (int i = 0; i < cand.num_candidates; i++) {
		bool highlighted = i == cand.highlighted_candidate_index;
		bufptr += snprintf(&buf[bufptr], sizeof buf - bufptr, " %s%d %s%s",
											 highlighted ? "[" : "", i + 1,
											 im_engine_candidate_get(state->engine, i),
											 highlighted ? "]" : "");
	}

	buf[bufptr] = 0;

	im_send_preedit(state, buf);
}

static void im_handle_key(struct wlpinyin_state *state,
													struct wlpinyin_key *keynode) {
	if (state->xkb_state == NULL)
		return;

	if (im_toggle(state->xkb_state, keynode->xkb_keysym, keynode->pressed)) {
		wlpinyin_err("toggle");
		im_engine_toggle(state->engine);
	}

	if (keynode->pressed) {
		bool handled =
				im_engine_key(state->engine, keynode->xkb_keysym,
											xkb_state_serialize_mods(state->xkb_state,
																							 XKB_STATE_MODS_EFFECTIVE |
																									 XKB_STATE_LAYOUT_EFFECTIVE));
		if (!handled) {
#ifndef NDEBUG
			char buf[512] = {};
			xkb_keysym_get_name(keynode->xkb_keysym, buf, sizeof buf);
			wlpinyin_dbg("upd_kbd[%s]: keycode %02x, keysym %02x", buf,
									 keynode->xkb_keycode, keynode->xkb_keysym);
#endif
			zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																	keynode->keycode,
																	WL_KEYBOARD_KEY_STATE_PRESSED);
			zwp_virtual_keyboard_v1_key(state->virtual_keyboard, get_miliseconds(),
																	keynode->keycode,
																	WL_KEYBOARD_KEY_STATE_RELEASED);
		} else {
			im_panel_update(state);
			const char *commit = im_engine_commit_text(state->engine);
			if (strlen(commit) != 0)
				im_send_text(state, commit);

			zwp_input_method_v2_commit(state->input_method, state->im_serial);
		}
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
	wlpinyin_dbg("ev_keymap: format %d, size %d, fd %d", format, size, fd);

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
	keynode.xkb_keycode = key + 8;
	keynode.xkb_keysym =
			xkb_state_key_get_one_sym(state->xkb_state, keynode.xkb_keycode);
	keynode.pressed = kstate == WL_KEYBOARD_KEY_STATE_PRESSED;

	xkb_state_update_key(state->xkb_state, keynode.xkb_keycode,
											 keynode.pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

#ifndef NDEBUG
	char buf[512] = {};
	xkb_keysym_get_name(keynode.xkb_keysym, buf, sizeof buf);
	wlpinyin_dbg("ev_key[%s]: keycode %02x, keysym %02x, serial %d, time %d, %s",
							 buf, keynode.xkb_keycode, keynode.xkb_keysym, serial, time,
							 keynode.pressed ? "pressed" : "released");
#endif

	// reset repeat key
	if (keynode.pressed &&
			xkb_keymap_key_repeats(state->xkb_keymap, keynode.xkb_keycode) == 1) {
		state->im_repeat_key = key;
		struct itimerspec timer = {
				.it_value = {.tv_nsec = state->im_repeat_delay * 1000000},
				.it_interval = {.tv_nsec = 1000000000 / state->im_repeat_rate},
		};
		timerfd_settime(state->timerfd, 0, &timer, NULL);
	} else {
		state->im_repeat_key = UINT32_MAX;
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
			"ev_modifiers: serial %d, depressed %d, latched %d, locked %d, group "
			"%d",
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
	wlpinyin_dbg("ev_repeat: rate %d, delay %d", rate, delay);
	state->im_repeat_delay = (uint32_t)delay;
	state->im_repeat_rate = (uint32_t)rate;
}

static void handle_reset(void *data,
												 struct zwp_input_method_v2 *zwp_input_method_v2) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	wlpinyin_dbg("ev_reset");
	im_engine_reset(state->engine);
}

static void handle_done(void *data,
												struct zwp_input_method_v2 *zwp_input_method_v2) {
	UNUSED(zwp_input_method_v2);
	struct wlpinyin_state *state = data;
	state->im_serial++;
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

	state->im_serial = 0;

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

	/*
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
	*/

	static const struct zwp_input_method_v2_listener im_listener = {
			.activate = handle_reset,
			.deactivate = handle_reset,
			.surrounding_text = noop,
			.text_change_cause = noop,
			.content_type = noop,
			.done = handle_done,
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
			if (wl_display_roundtrip(state->display) == -1) {
				break;
			}
		} else if (fds[fd_repeat].revents & POLLIN) {
			uint64_t tick;
			read(fds[fd_repeat].fd, &tick, sizeof tick);
			struct wlpinyin_key keynode = {
					.keycode = state->im_repeat_key,
					.xkb_keycode = state->im_repeat_key + 8,
					.xkb_keysym = xkb_state_key_get_one_sym(state->xkb_state,
																									state->im_repeat_key + 8),
			};
			for (; tick != 0; tick--) {
				// repeat may be stopped if there are new events
				if (wl_display_roundtrip(state->display) == -1)
					break;
				if (keynode.keycode == UINT32_MAX)
					break;
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
	/*
	if (state->popup_surface)
		zwp_input_popup_surface_v2_destroy(state->popup_surface);
	if (state->popup_surface_wl)
		wl_surface_destroy(state->popup_surface_wl);
	*/
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
