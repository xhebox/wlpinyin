#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "wlpinyin.h"

void noop() {}

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
	}
}

int main(int argc, char *argv[]) {
	sigset_t sigset;

	int r = sigemptyset(&sigset);
	if (r != 0) {
		wlpinyin_err("failed to empty sigset");
		return EXIT_FAILURE;
	}

	r = sigaddset(&sigset, SIGINT);
	if (r != 0) {
		wlpinyin_err("failed to add SIGINT to sigset");
		return EXIT_FAILURE;
	}

	r = sigprocmask(SIG_BLOCK, &sigset, NULL);
	if (r != 0) {
		wlpinyin_err("failed to block sigset");
		return EXIT_FAILURE;
	}

	int sigfd = signalfd(-1, &sigset, 0);
	if (sigfd == -1) {
		wlpinyin_err("failed to alloc a signalfd");
		return EXIT_FAILURE;
	}

	struct wlpinyin_state *state = calloc(1, sizeof(struct wlpinyin_state));
	if (state == NULL) {
		wlpinyin_err("failed to calloc state");
		return EXIT_FAILURE;
	}

	state->display = wl_display_connect(NULL);
	if (state->display == NULL) {
		wlpinyin_err("failed to create display");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(state->display);

	static const struct wl_registry_listener registry_listener = {
			.global = handle_global,
			.global_remove = NULL,
	};
	wl_registry_add_listener(registry, &registry_listener, state);
	wl_display_roundtrip(state->display);

	if (state->input_method_manager == NULL ||
			state->virtual_keyboard_manager == NULL) {
		wlpinyin_err("required wayland interface not available");
		return EXIT_FAILURE;
	}

	im_setup(state);

	struct pollfd fds[4] = {};

	fds[0].fd = wl_display_get_fd(state->display);
	fds[0].events = POLLIN;

	fds[1].fd = im_repeat_timerfd(state);
	fds[1].events = POLLIN;

	fds[2].fd = sigfd;
	fds[2].events = POLLIN;

	fds[3].fd = im_event_fd(state);
	fds[3].events = POLLIN;

	while (im_running(state) && poll(fds, sizeof fds / sizeof fds[0], -1) != -1) {
		if (fds[0].revents & POLLIN) {
			fds[0].revents = 0;
			if (wl_display_dispatch(state->display) == -1) {
				break;
			}
		}

		if (fds[2].revents & POLLIN) {
			fds[2].revents = 0;
			struct signalfd_siginfo info = {};
			read(sigfd, &info, sizeof(info));

			switch (info.ssi_signo) {
			case SIGINT:
				im_exit(state);
				break;
			}
		}

		if (fds[3].revents & POLLIN) {
			fds[3].revents = 0;
			uint64_t t = 0;
			read(fds[3].fd, &t, sizeof t);
			im_handle(state);
		}

		if (fds[1].revents & POLLIN) {
			fds[1].revents = 0;
			uint64_t t = 0;
			read(fds[1].fd, &t, sizeof t);
			im_repeat(state, t);
		}
	}

	// clear all events
	im_handle(state);
	im_destroy(state);
	return EXIT_SUCCESS;
}
