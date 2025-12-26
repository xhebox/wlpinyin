#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "wlpinyin.h"

int main(int argc, char *argv[]) {
	UNUSED(argc);
	UNUSED(argv);
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

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		wlpinyin_err("failed to create display");
		return EXIT_FAILURE;
	}

	struct wlpinyin_state *state = im_setup(sigfd, display);
	if (state == NULL) {
		wlpinyin_err("failed to setup state");
		return EXIT_FAILURE;
	}

	if (im_loop(state) != 0) {
		wlpinyin_err("stopped unexpectedly");
	}

	return im_destroy(state);
}
