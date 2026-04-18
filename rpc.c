#include <glib.h>
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

	if (listen(fd, 4) == -1) {
		wlpinyin_err("failed to listen on socket");
		close(fd);
		g_free(path);
		return -1;
	}

	state->rpc_fd = fd;
	state->rpc_socket_path = path;
	wlpinyin_dbg("rpc socket listening at %s", path);
	return 0;
}

void rpc_handle(struct wlpinyin_state *state) {
	int client = accept4(state->rpc_fd, NULL, NULL, SOCK_NONBLOCK);
	if (client == -1) {
		return;
	}

	char buf[256] = {0};
	ssize_t n = read(client, buf, sizeof(buf) - 1);
	if (n <= 0) {
		close(client);
		return;
	}

	// Strip trailing \r\n or \n
	while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) {
		buf[--n] = '\0';
	}

	if (strcmp(buf, "enable") == 0) {
		state->im_enabled = true;
		write(client, "ok\n", 3);
	} else if (strcmp(buf, "disable") == 0) {
		state->im_enabled = false;
		write(client, "ok\n", 3);
	} else if (strcmp(buf, "toggle") == 0) {
		state->im_enabled = !state->im_enabled;
		write(client, "ok\n", 3);
	} else {
		write(client, "error: unknown command\n", 23);
	}

	close(client);
}

void rpc_destroy(struct wlpinyin_state *state) {
	if (state->rpc_fd != -1) {
		close(state->rpc_fd);
	}
	if (state->rpc_socket_path != NULL) {
		unlink(state->rpc_socket_path);
		g_free(state->rpc_socket_path);
	}
}
