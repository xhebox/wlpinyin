#include <glib.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "wlpinyin.h"

int rpc_init(struct wlpinyin_state *state) {
	const char *dir = g_get_user_runtime_dir();
	if (dir == NULL) {
		wlpinyin_err("XDG_RUNTIME_DIR not set");
		return -1;
	}

	char *path = g_strdup_printf("%s/wlpinyin.sock", dir);
	if (path == NULL) {
		wlpinyin_err("failed to allocate socket path");
		return -1;
	}

	// Remove stale socket
	unlink(path);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd == -1) {
		wlpinyin_err("failed to create socket");
		g_free(path);
		return -1;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	size_t path_len = strlen(path);
	if (path_len >= sizeof(addr.sun_path)) {
		wlpinyin_err("socket path too long");
		close(fd);
		g_free(path);
		return -1;
	}
	memcpy(addr.sun_path, path, path_len + 1);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		wlpinyin_err("failed to bind socket");
		close(fd);
		g_free(path);
		return -1;
	}

	// Allow multiple pending connections in backlog
	if (listen(fd, 5) == -1) {
		wlpinyin_err("failed to listen on socket");
		close(fd);
		g_free(path);
		return -1;
	}

	state->rpc_fd = fd;
	state->rpc_socket_path = path;
	state->rpc_client = -1;

	wlpinyin_dbg("rpc socket listening at %s", path);
	return 0;
}

static void rpc_close_client(struct wlpinyin_state *state) {
	if (state->rpc_client != -1) {
		close(state->rpc_client);
		state->rpc_client = -1;
		wlpinyin_dbg("rpc client disconnected");
	}
}

void rpc_accept(struct wlpinyin_state *state) {
	// Only accept if no existing client
	if (state->rpc_client != -1) {
		return;
	}

	int client = accept4(state->rpc_fd, NULL, NULL, SOCK_NONBLOCK);
	if (client != -1) {
		state->rpc_client = client;
		wlpinyin_dbg("rpc client connected");
	}
}

void rpc_handle_client_data(struct wlpinyin_state *state) {
	if (state->rpc_client == -1)
		return;

	char buf[256] = {0};
	ssize_t n = read(state->rpc_client, buf, sizeof(buf) - 1);

	if (n <= 0) {
		// Connection closed or error
		rpc_close_client(state);
		return;
	}

	// Strip trailing \r\n or \n
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
		buf[--n] = '\0';
	}

	wlpinyin_dbg("rpc client command: %s", buf);
	if (strcmp(buf, "enable") == 0) {
		// Enable Chinese input: turn off ascii_mode
		im_engine_set_ascii_mode(state->engine, false);
		write(state->rpc_client, "ok\n", 3);
	} else if (strcmp(buf, "disable") == 0) {
		// Disable Chinese input: turn on ascii_mode
		im_engine_set_ascii_mode(state->engine, true);
		write(state->rpc_client, "ok\n", 3);
	} else if (strcmp(buf, "toggle") == 0) {
		// Toggle ascii_mode
		im_engine_toggle(state->engine);
		write(state->rpc_client, "ok\n", 3);
	} else if (strcmp(buf, "status") == 0) {
		// Query current status
		bool ascii_mode = im_engine_get_ascii_mode(state->engine);
		const char *status = ascii_mode ? "disable\n" : "enable\n";
		write(state->rpc_client, status, strlen(status));
	} else {
		write(state->rpc_client, "error: unknown command\n", 23);
	}
}

void rpc_destroy(struct wlpinyin_state *state) {
	rpc_close_client(state);

	if (state->rpc_fd != -1) {
		close(state->rpc_fd);
	}
	if (state->rpc_socket_path != NULL) {
		unlink(state->rpc_socket_path);
		g_free(state->rpc_socket_path);
	}
}
