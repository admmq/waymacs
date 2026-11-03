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
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int listen_fd = -1, client_fd = -1;
static wl_event_source *listen_source, *client_source;
static std::string socket_path, received_text, unsent_text;

bool ipc_connected()
{
	return client_fd >= 0;
}

static void ipc_close()
{
	if (client_source)
		wl_event_source_remove(client_source);
	if (client_fd >= 0)
		close(client_fd);
	client_source = nullptr;
	client_fd = -1;
	received_text.clear();
	unsent_text.clear();
}

static void ipc_flush()
{
	while (client_fd >= 0 && !unsent_text.empty()) {
		ssize_t sent = send(client_fd, unsent_text.data(), unsent_text.size(), MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && errno == EAGAIN) {
			/* finish when the socket is writable again */
			wl_event_source_fd_update(client_source, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
			return;
		}
		if (sent < 0) {
			wlr_log_errno(WLR_ERROR, "ipc: send");
			ipc_close();
			return;
		}
		unsent_text.erase(0, sent);
	}
}

void ipc_send(const char *format, ...)
{
	if (client_fd < 0)
		return;
	va_list args;
	va_start(args, format);
	char *text;
	int length = vasprintf(&text, format, args);
	va_end(args);
	if (length < 0)
		return;
	unsent_text.append(text, length).push_back('\n');
	free(text);
	ipc_flush();
}

/* TEXT as a Lisp string literal, in one of four rotating buffers. */
const char *lisp_string(const char *text)
{
	static std::string buffers[4];
	static int next_buffer;
	std::string &result = buffers[next_buffer++ % 4];
	result = '"';
	for (; text && *text; text++) {
		unsigned char character = *text;
		if (character == '"' || character == '\\')
			result += '\\';
		result += character < 0x20 ? ' ' : character;
	}
	result += '"';
	return result.c_str();
}

/* Key specs look like MODIFIERS:cCODEPOINT or MODIFIERS:sKEYSYM-NAME. */
static bool parse_key_spec(const std::string &text, KeySpec &key, bool lowercase)
{
	char *rest;
	unsigned long modifiers = strtoul(text.c_str(), &rest, 10);
	if (*rest != ':')
		return false;
	rest++;
	if (*rest == 'c')
		key.keysym = xkb_utf32_to_keysym(strtoul(rest + 1, nullptr, 10));
	else if (*rest == 's')
		key.keysym = xkb_keysym_from_name(rest + 1, XKB_KEYSYM_CASE_INSENSITIVE);
	else
		return false;
	if (key.keysym == XKB_KEY_NoSymbol)
		return false;
	if (lowercase)
		key.keysym = xkb_keysym_to_lower(key.keysym);
	key.modifiers = modifiers & MODIFIER_MASK;
	return true;
}

static std::vector<KeySpec> parse_key_list(const std::vector<std::string> &words, size_t first,
					   bool lowercase)
{
	std::vector<KeySpec> keys;
	for (size_t i = first; i < words.size(); i++) {
		KeySpec key;
		if (parse_key_spec(words[i], key, lowercase))
			keys.push_back(key);
		else
			wlr_log(WLR_ERROR, "ipc: bad key spec `%s'", words[i].c_str());
	}
	return keys;
}

/* simulation-keys FROM=TO[,TO...] ... */
static void parse_simulation_keys(const std::vector<std::string> &words)
{
	server.simulation_keys.clear();
	for (size_t i = 1; i < words.size(); i++) {
		size_t equals = words[i].find('=');
		SimulationKey simulation;
		if (equals == std::string::npos ||
		    !parse_key_spec(words[i].substr(0, equals), simulation.from, true))
			continue;
		std::istringstream targets(words[i].substr(equals + 1));
		std::string target;
		KeySpec key;
		while (std::getline(targets, target, ','))
			if (parse_key_spec(target, key, false))
				simulation.to.push_back(key);
		if (!simulation.to.empty())
			server.simulation_keys.push_back(simulation);
	}
}

/* words NAME=VALUE from FIRST on: output-settings, pointer-settings */
static Settings parse_settings(const std::vector<std::string> &words, size_t first)
{
	Settings settings;
	for (size_t i = first; i < words.size(); i++) {
		size_t equals = words[i].find('=');
		if (equals != std::string::npos)
			settings.values[words[i].substr(0, equals)] = words[i].substr(equals + 1);
	}
	return settings;
}

static void handle_command(const std::string &line)
{
	std::vector<std::string> words;
	std::istringstream stream(line);
	for (std::string word; stream >> word;)
		words.push_back(word);
	if (words.empty())
		return;
	const std::string &command = words[0];
	size_t argc = words.size();
	auto number = [&words](size_t i) { return atoi(words[i].c_str()); };
	auto id = [&words](size_t i) { return (uint32_t)strtoul(words[i].c_str(), nullptr, 10); };
	/* "-" stands for an empty string */
	auto text = [&words](size_t i) { return words[i] == "-" ? std::string() : words[i]; };
	View *view = argc > 1 ? view_by_id(id(1)) : nullptr;
	bool app = view && view->mapped && !view->is_frame;
	bool frame = view && view->is_frame;

	if (command == "hello" && argc > 1) {
		emacs_hello(number(1));
	} else if (command == "layout") {
		layout_begin();
		for (size_t i = 1; i + 5 < argc; i += 6)
			layout_add(id(i), id(i + 1), number(i + 2), number(i + 3),
				   number(i + 4), number(i + 5));
		layout_end();
	} else if (command == "focus") {
		if (view && view->mapped)
			focus_view(view, true);
	} else if (command == "global-keys") {
		server.global_keys = parse_key_list(words, 1, true);
	} else if (command == "prefix-keys") {
		server.prefix_keys = parse_key_list(words, 1, true);
	} else if (command == "simulation-keys") {
		parse_simulation_keys(words);
	} else if (command == "mode" && app && argc > 2) {
		view->char_mode = words[2] == "char";
	} else if (command == "fullscreen" && app && argc > 2) {
		view_set_fullscreen(view, number(2));
	} else if (command == "float" && app && argc > 2) {
		view_set_floating(view, number(2));
	} else if (command == "assign" && app && argc > 2) {
		/* a floating window moves to another workspace (0: none yet) */
		view_set_frame(view, view_by_id(id(2)));
	} else if (command == "workspaces") {
		/* workspaces FRAME NUMBER ...: frames left out have none; frame 0
		 * stands for a workspace not shown yet (no surface) */
		for (View *other : server.views)
			other->workspace = 0;
		server.unshown_workspace_tags = 0;
		for (size_t i = 1; i + 1 < argc; i += 2) {
			int workspace = number(i + 1);
			View *other = view_by_id(id(i));
			if (id(i) == 0 && workspace >= 1 && workspace <= 32)
				server.unshown_workspace_tags |= 1u << (workspace - 1);
			else if (other && other->is_frame)
				other->workspace = workspace;
		}
		bar_update();
	} else if (command == "minibuffer" && frame && argc > 2) {
		/* where the frame's minibuffer starts, for fullscreen windows */
		view->minibuffer_top = number(2);
		arrange();
	} else if (command == "frame-output" && frame && argc > 2) {
		if (Output *output = output_by_name(words[2].c_str(), true))
			frame_move_to_output(view, output);
	} else if (command == "output-enable" && argc > 2) {
		/* output-enable OUTPUT 0|1: turn a monitor off or on */
		if (Output *output = output_by_name(words[1].c_str(), false))
			output_set_enabled(output, number(2) != 0);
	} else if (command == "frame-place" && frame && argc > 2) {
		/* frame-place FRAME OUTPUT: like frame-output, without focusing it */
		if (Output *output = output_by_name(words[2].c_str(), true))
			frame_set_output(view, output);
	} else if ((command == "close" || command == "kill") && app) {
		view->close(command == "kill");
	} else if (command == "key" && app) {
		std::vector<KeySpec> keys = parse_key_list(words, 2, false);
		if (server.focused_view != view)
			focus_view(view, false);
		inject_keys(keys);
	} else if (command == "xkb" && argc > 3) {
		server.xkb_layout = text(1);
		server.xkb_variant = text(2);
		server.xkb_options = text(3);
		keyboards_reconfigure();
	} else if (command == "repeat" && argc > 2) {
		server.repeat_rate = number(1);
		server.repeat_delay = number(2);
		keyboards_reconfigure();
	} else if (command == "output-settings" && argc > 1) {
		/* output-settings OUTPUT NAME=VALUE...: waymacs-output-settings */
		output_configure(words[1], parse_settings(words, 2));
	} else if (command == "pointer-settings" && argc > 1 &&
		   (words[1] == "touchpad" || words[1] == "mouse")) {
		/* pointer-settings touchpad|mouse NAME=VALUE...: waymacs-pointer-settings */
		(words[1] == "touchpad" ? server.touchpad_settings : server.mouse_settings) =
			parse_settings(words, 2);
		pointers_reconfigure();
	} else if (command == "quit") {
		wl_display_terminate(server.display);
	} else {
		wlr_log(WLR_DEBUG, "ipc: ignored command `%s'", command.c_str());
	}
}

static int handle_ipc_readable(int fd, uint32_t mask, void *)
{
	if (mask & WL_EVENT_WRITABLE) {
		ipc_flush();
		if (client_source && unsent_text.empty())	/* all sent: stop waiting */
			wl_event_source_fd_update(client_source, WL_EVENT_READABLE);
	}
	if (client_fd < 0)
		return 0;	/* the flush failed */
	if (mask & WL_EVENT_READABLE) {
		char buffer[4096];
		for (;;) {
			ssize_t received = read(fd, buffer, sizeof buffer);
			if (received < 0 && errno == EINTR)
				continue;
			if (received < 0 && errno == EAGAIN)
				break;
			if (received <= 0) {
				wlr_log(WLR_INFO, "ipc: Emacs disconnected");
				ipc_close();
				return 0;
			}
			received_text.append(buffer, received);
		}
		size_t start = 0, newline;
		while ((newline = received_text.find('\n', start)) != std::string::npos) {
			std::string line = received_text.substr(start, newline - start);
			start = newline + 1;
			handle_command(line);
			if (client_fd < 0)
				return 0;
		}
		received_text.erase(0, start);
	} else if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		ipc_close();
	}
	return 0;
}

static int handle_ipc_connection(int fd, uint32_t mask, void *)
{
	int new_fd = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (new_fd < 0)
		return 0;
	ucred credentials;
	socklen_t length = sizeof credentials;
	if (getsockopt(new_fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0 ||
	    credentials.uid != getuid()) {
		close(new_fd);
		return 0;
	}
	if (client_fd >= 0) {
		/* Only the session's Emacs manages windows; another Emacs
		 * started in the session is just an application. */
		if (credentials.pid != server.emacs_pid) {
			wlr_log(WLR_INFO, "ipc: refused a second Emacs (pid %d)", credentials.pid);
			close(new_fd);
			return 0;
		}
		wlr_log(WLR_INFO, "ipc: Emacs reconnected");
		ipc_close();
	}
	client_fd = new_fd;
	client_source = wl_event_loop_add_fd(server.event_loop, client_fd, WL_EVENT_READABLE,
					     handle_ipc_readable, nullptr);
	wlr_log(WLR_INFO, "ipc: Emacs connected (pid %d)", credentials.pid);
	return 0;
}

void ipc_init(const char *display_name)
{
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	socket_path = std::string(runtime_dir ? runtime_dir : "/tmp") + "/waymacs-" +
		display_name + ".sock";
	if (socket_path.size() >= sizeof address.sun_path) {
		wlr_log(WLR_ERROR, "ipc: socket path too long");
		return;
	}
	socket_path.copy(address.sun_path, socket_path.size());
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	unlink(socket_path.c_str());
	if (fd < 0 || bind(fd, (sockaddr *)&address, sizeof address) < 0 || listen(fd, 4) < 0) {
		wlr_log_errno(WLR_ERROR, "ipc: cannot listen on %s", socket_path.c_str());
		if (fd >= 0)
			close(fd);
		return;
	}
	listen_fd = fd;
	listen_source = wl_event_loop_add_fd(server.event_loop, listen_fd, WL_EVENT_READABLE,
					     handle_ipc_connection, nullptr);
	setenv("WAYMACS_SOCKET", socket_path.c_str(), 1);
}

void ipc_finish()
{
	ipc_close();
	if (listen_source)
		wl_event_source_remove(listen_source);
	if (listen_fd >= 0) {
		close(listen_fd);
		unlink(socket_path.c_str());
	}
}
