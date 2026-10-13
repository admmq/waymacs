/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* ipc.cpp - the connection to Emacs.
 *
 * One Unix stream socket ($WAYMACS_SOCKET), one client: Emacs.
 *   Emacs -> waymacs: one command per line, space separated words.
 *   waymacs -> Emacs: one S-expression per line, `(EVENT :key value ...)'.
 * The protocol is documented in README.md.
 */
#include "waymacs.h"
#include <errno.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

bool ipc_connected(void)
{
	return server.ipc_fd >= 0;
}

static void ipc_close(void)
{
	if (server.ipc_source)
		wl_event_source_remove(server.ipc_source);
	if (server.ipc_fd >= 0)
		close(server.ipc_fd);
	server.ipc_source = NULL;
	server.ipc_fd = -1;
	server.ipc_input_length = server.ipc_output_length = 0;
}

static void ipc_flush(void)
{
	while (server.ipc_fd >= 0 && server.ipc_output_length > 0) {
		ssize_t sent = send(server.ipc_fd, server.ipc_output,
				    server.ipc_output_length, MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && errno == EAGAIN) {
			/* finish when the socket is writable again */
			wl_event_source_fd_update(server.ipc_source,
						  WL_EVENT_READABLE | WL_EVENT_WRITABLE);
			return;
		}
		if (sent < 0) {
			wlr_log_errno(WLR_ERROR, "ipc: send");
			ipc_close();
			return;
		}
		memmove(server.ipc_output, server.ipc_output + sent,
			server.ipc_output_length - sent);
		server.ipc_output_length -= sent;
	}
	if (server.ipc_source)
		wl_event_source_fd_update(server.ipc_source, WL_EVENT_READABLE);
}

/* Make *BUFFER (of *CAPACITY bytes) hold at least NEEDED bytes. */
static void buffer_reserve(char **buffer, size_t *capacity, size_t needed)
{
	if (needed <= *capacity)
		return;
	size_t new_capacity = *capacity ? *capacity : 4096;
	while (new_capacity < needed)
		new_capacity *= 2;
	*buffer = (char *)realloc(*buffer, new_capacity);
	if (!*buffer)
		abort();
	*capacity = new_capacity;
}

void ipc_send(const char *format, ...)
{
	if (server.ipc_fd < 0)
		return;
	va_list args;
	va_start(args, format);
	int length = vsnprintf(NULL, 0, format, args);
	va_end(args);
	if (length < 0)
		return;
	buffer_reserve(&server.ipc_output, &server.ipc_output_capacity,
		       server.ipc_output_length + length + 2);
	va_start(args, format);
	vsnprintf(server.ipc_output + server.ipc_output_length, length + 1, format, args);
	va_end(args);
	server.ipc_output_length += length;
	server.ipc_output[server.ipc_output_length++] = '\n';
	ipc_flush();
}

/* TEXT as a Lisp string literal, in one of four rotating buffers. */
const char *lisp_string(const char *text)
{
	static char buffers[4][1024];
	static int next_buffer;
	char *result = buffers[next_buffer++ % 4];
	size_t length = 0;
	result[length++] = '"';
	for (; text && *text && length < sizeof buffers[0] - 3; text++) {
		unsigned char character = *text;
		if (character == '"' || character == '\\')
			result[length++] = '\\';
		result[length++] = character < 0x20 ? ' ' : character;
	}
	result[length++] = '"';
	result[length] = 0;
	return result;
}

/* Key specs look like MODIFIERS:cCODEPOINT or MODIFIERS:sKEYSYM-NAME. */
bool parse_key_spec(const char *text, struct key_spec *key, bool lowercase)
{
	char *rest;
	unsigned long modifiers = strtoul(text, &rest, 10);
	if (*rest != ':')
		return false;
	rest++;
	if (*rest == 'c')
		key->keysym = xkb_utf32_to_keysym(strtoul(rest + 1, NULL, 10));
	else if (*rest == 's')
		key->keysym = xkb_keysym_from_name(rest + 1, XKB_KEYSYM_CASE_INSENSITIVE);
	else
		return false;
	if (key->keysym == XKB_KEY_NoSymbol)
		return false;
	if (lowercase)
		key->keysym = xkb_keysym_to_lower(key->keysym);
	key->modifiers = modifiers & MODIFIER_MASK;
	return true;
}

static int parse_key_list(int argc, char **argv, struct key_spec *keys)
{
	int count = 0;
	for (int i = 1; i < argc && count < MAX_KEYS; i++) {
		if (parse_key_spec(argv[i], &keys[count], true))
			count++;
		else
			wlr_log(WLR_ERROR, "ipc: bad key spec `%s'", argv[i]);
	}
	return count;
}

/* simulation-keys FROM=TO[,TO...] ... */
static void parse_simulation_keys(int argc, char **argv)
{
	server.simulation_key_count = 0;
	for (int i = 1; i < argc && server.simulation_key_count < MAX_KEYS; i++) {
		struct simulation_key *simulation =
			&server.simulation_keys[server.simulation_key_count];
		char *equals = strchr(argv[i], '=');
		if (!equals)
			continue;
		*equals = 0;
		if (!parse_key_spec(argv[i], &simulation->from, true))
			continue;
		simulation->count = 0;
		char *position = NULL;
		for (char *target = strtok_r(equals + 1, ",", &position);
		     target && simulation->count < MAX_SIMULATED_KEYS;
		     target = strtok_r(NULL, ",", &position))
			if (parse_key_spec(target, &simulation->to[simulation->count], false))
				simulation->count++;
		if (simulation->count > 0)
			server.simulation_key_count++;
	}
}

static struct view *view_argument(int argc, char **argv, int index)
{
	return index < argc ? view_by_id(strtoul(argv[index], NULL, 10)) : NULL;
}

/* "-" stands for an empty string */
static void copy_argument(char *destination, size_t size, const char *argument)
{
	snprintf(destination, size, "%s", strcmp(argument, "-") ? argument : "");
}

static void handle_command(char *line)
{
	static char *argv[2048];
	int argc = 0;
	char *position = NULL;
	for (char *word = strtok_r(line, " \t\r", &position); word && argc < 2048;
	     word = strtok_r(NULL, " \t\r", &position))
		argv[argc++] = word;
	if (argc == 0)
		return;
	const char *command = argv[0];
	struct view *view = view_argument(argc, argv, 1);
	bool app = view && view->mapped && !view->is_frame;

	if (!strcmp(command, "hello") && argc > 1) {
		emacs_hello(atoi(argv[1]));
	} else if (!strcmp(command, "layout")) {
		layout_begin();
		for (int i = 1; i + 5 < argc; i += 6)
			layout_add(strtoul(argv[i], NULL, 10), strtoul(argv[i + 1], NULL, 10),
				   atoi(argv[i + 2]), atoi(argv[i + 3]),
				   atoi(argv[i + 4]), atoi(argv[i + 5]));
		layout_end();
	} else if (!strcmp(command, "focus")) {
		if (view && view->mapped)
			focus_view(view, true);
	} else if (!strcmp(command, "global-keys")) {
		server.global_key_count = parse_key_list(argc, argv, server.global_keys);
	} else if (!strcmp(command, "prefix-keys")) {
		server.prefix_key_count = parse_key_list(argc, argv, server.prefix_keys);
	} else if (!strcmp(command, "simulation-keys")) {
		parse_simulation_keys(argc, argv);
	} else if (!strcmp(command, "mode") && app && argc > 2) {
		view->char_mode = !strcmp(argv[2], "char");
	} else if (!strcmp(command, "fullscreen") && app && argc > 2) {
		view_set_fullscreen(view, atoi(argv[2]));
	} else if (!strcmp(command, "float") && app && argc > 2) {
		view_set_floating(view, atoi(argv[2]));
	} else if (!strcmp(command, "assign") && app && argc > 2) {
		/* a floating window moves to another workspace (0: none yet) */
		view_set_frame(view, view_argument(argc, argv, 2));
	} else if (!strcmp(command, "workspaces")) {
		/* workspaces FRAME NUMBER ...: frames left out have none; frame 0
		 * stands for a workspace not shown yet (no surface) */
		struct view *frame;
		wl_list_for_each(frame, &server.views, link)
			frame->workspace = 0;
		server.unshown_workspace_tags = 0;
		for (int i = 1; i + 1 < argc; i += 2) {
			int number = atoi(argv[i + 1]);
			unsigned long frame_id = strtoul(argv[i], NULL, 10);
			if (frame_id == 0 && number >= 1 && number <= 32)
				server.unshown_workspace_tags |= 1u << (number - 1);
			else if ((frame = view_by_id(frame_id)) && frame->is_frame)
				frame->workspace = number;
		}
		bar_update();
	} else if (!strcmp(command, "minibuffer") && view && view->is_frame && argc > 2) {
		/* where the frame's minibuffer starts, for fullscreen windows */
		view->minibuffer_top = atoi(argv[2]);
		arrange();
	} else if (!strcmp(command, "frame-output") && view && view->is_frame && argc > 2) {
		struct output *output = output_by_name(argv[2], true);
		if (output)
			frame_move_to_output(view, output);
	} else if (!strcmp(command, "output-enable") && argc > 2) {
		/* output-enable OUTPUT 0|1: turn a monitor off or on */
		struct output *output = output_by_name(argv[1], false);
		if (output)
			output_set_enabled(output, atoi(argv[2]) != 0);
	} else if (!strcmp(command, "frame-place") && view && view->is_frame && argc > 2) {
		/* frame-place FRAME OUTPUT: like frame-output, without focusing it */
		struct output *output = output_by_name(argv[2], true);
		if (output)
			frame_set_output(view, output);
	} else if (!strcmp(command, "close") && app) {
		view_close(view, false);
	} else if (!strcmp(command, "kill") && app) {
		view_close(view, true);
	} else if (!strcmp(command, "key") && app) {
		struct key_spec keys[MAX_SIMULATED_KEYS];
		int count = 0;
		for (int i = 2; i < argc && count < MAX_SIMULATED_KEYS; i++)
			if (parse_key_spec(argv[i], &keys[count], false))
				count++;
		if (server.focused_view != view)
			focus_view(view, false);
		inject_keys(keys, count);
	} else if (!strcmp(command, "xkb") && argc > 3) {
		copy_argument(server.xkb_layout, sizeof server.xkb_layout, argv[1]);
		copy_argument(server.xkb_variant, sizeof server.xkb_variant, argv[2]);
		copy_argument(server.xkb_options, sizeof server.xkb_options, argv[3]);
		keyboards_reconfigure();
	} else if (!strcmp(command, "repeat") && argc > 2) {
		server.repeat_rate = atoi(argv[1]);
		server.repeat_delay = atoi(argv[2]);
		keyboards_reconfigure();
	} else if (!strcmp(command, "quit")) {
		wl_display_terminate(server.display);
	} else {
		wlr_log(WLR_DEBUG, "ipc: ignored command `%s'", command);
	}
}

static int handle_ipc_readable(int client_fd, uint32_t mask, void *data)
{
	if (mask & WL_EVENT_WRITABLE)
		ipc_flush();
	if (mask & WL_EVENT_READABLE) {
		for (;;) {
			buffer_reserve(&server.ipc_input, &server.ipc_input_capacity,
				       server.ipc_input_length + 4096);
			ssize_t received = read(client_fd, server.ipc_input + server.ipc_input_length,
						server.ipc_input_capacity - server.ipc_input_length);
			if (received < 0 && errno == EINTR)
				continue;
			if (received < 0 && errno == EAGAIN)
				break;
			if (received <= 0) {
				wlr_log(WLR_INFO, "ipc: Emacs disconnected");
				ipc_close();
				return 0;
			}
			server.ipc_input_length += received;
		}
		char *line = server.ipc_input;
		char *end = server.ipc_input + server.ipc_input_length;
		char *newline;
		while ((newline = (char *)memchr(line, '\n', end - line))) {
			*newline = 0;
			handle_command(line);
			if (server.ipc_fd < 0)
				return 0;
			line = newline + 1;
		}
		server.ipc_input_length = end - line;
		memmove(server.ipc_input, line, server.ipc_input_length);
	} else if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		ipc_close();
	}
	return 0;
}

static int handle_ipc_connection(int listen_fd, uint32_t mask, void *data)
{
	int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (client_fd < 0)
		return 0;
	struct ucred credentials;
	socklen_t length = sizeof credentials;
	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0 ||
	    credentials.uid != getuid()) {
		close(client_fd);
		return 0;
	}
	if (server.ipc_fd >= 0) {
		/* Only the session's Emacs manages windows; another Emacs
		 * started in the session is just an application. */
		if (credentials.pid != server.emacs_pid) {
			wlr_log(WLR_INFO, "ipc: refused a second Emacs (pid %d)", credentials.pid);
			close(client_fd);
			return 0;
		}
		wlr_log(WLR_INFO, "ipc: Emacs reconnected");
		ipc_close();
	}
	server.ipc_fd = client_fd;
	server.ipc_source = wl_event_loop_add_fd(server.event_loop, client_fd,
						 WL_EVENT_READABLE, handle_ipc_readable, NULL);
	wlr_log(WLR_INFO, "ipc: Emacs connected (pid %d)", credentials.pid);
	return 0;
}

void ipc_init(const char *display_name)
{
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	struct sockaddr_un address;
	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	int length = snprintf(address.sun_path, sizeof address.sun_path, "%s/waymacs-%s.sock",
			      runtime_dir ? runtime_dir : "/tmp", display_name);
	if (length < 0 || (size_t)length >= sizeof address.sun_path) {
		wlr_log(WLR_ERROR, "ipc: socket path too long");
		return;
	}
	snprintf(server.socket_path, sizeof server.socket_path, "%s", address.sun_path);
	int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	unlink(server.socket_path);
	if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&address, sizeof address) < 0 ||
	    listen(listen_fd, 4) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: cannot listen on %s", server.socket_path);
		if (listen_fd >= 0)
			close(listen_fd);
		return;
	}
	server.ipc_listen_fd = listen_fd;
	server.ipc_listen_source = wl_event_loop_add_fd(server.event_loop, listen_fd, WL_EVENT_READABLE,
							handle_ipc_connection, NULL);
	setenv("WAYMACS_SOCKET", server.socket_path, 1);
}

void ipc_finish(void)
{
	ipc_close();
	if (server.ipc_listen_source)
		wl_event_source_remove(server.ipc_listen_source);
	if (server.ipc_listen_fd >= 0) {
		close(server.ipc_listen_fd);
		unlink(server.socket_path);
	}
	free(server.ipc_input);
	free(server.ipc_output);
}
