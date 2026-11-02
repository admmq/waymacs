/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* main.cpp - setup, protocol globals, the startup command, shutdown. */
#include "waymacs.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

Server server;

void Server::listen(wl_signal *signal, Listener::Handler handler)
{
	listeners.push_back(std::make_unique<Listener>());
	listeners.back()->listen(signal, std::move(handler));
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [-d] [emacs lispdir | -s command | -- program [argument...]]\n"
		"  emacs lispdir  run EMACS -l LISPDIR/waymacs.el -f waymacs-enable\n"
		"  -s command     run the shell COMMAND\n"
		"  -- program     run PROGRAM with its arguments, without a shell\n"
		"                 (default: $WAYMACS_EMACS or \"emacs\");\n"
		"                 the session ends when it exits\n"
		"  -d             debug logging\n"
		"example: %s emacs /path/to/waymacs/lisp\n",
		program, program);
}

static void spawn(const std::vector<std::string> &command)
{
	std::vector<char *> command_argv;
	for (const std::string &word : command)
		command_argv.push_back(const_cast<char *>(word.c_str()));
	command_argv.push_back(nullptr);
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork");
		return;
	}
	if (pid == 0) {
		/* wl_event_loop_add_signal() blocked these in the parent */
		sigset_t signals;
		sigemptyset(&signals);
		sigprocmask(SIG_SETMASK, &signals, nullptr);
		setsid();
		execvp(command_argv[0], command_argv.data());
		fprintf(stderr, "waymacs: cannot run %s\n", command_argv[0]);
		_exit(127);
	}
	server.child_pid = pid;
	/* a guess (right unless a shell or wrapper starts Emacs); `hello'
	 * tells for sure */
	server.emacs_pid = pid;
	wlr_log(WLR_INFO, "started %s (pid %d)", command_argv[0], pid);
}

/* Only the startup command is ours to reap: wlroots waits for the
 * process that launches Xwayland itself. */
static int handle_sigchld(int signal_number, void *data)
{
	int status;
	if (server.child_pid <= 0 || waitpid(server.child_pid, &status, WNOHANG) <= 0)
		return 0;
	wlr_log(WLR_INFO, "startup command exited (status %d), quitting",
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	server.child_pid = 0;
	wl_display_terminate(server.display);
	return 0;
}

/* Emacs normally connects within a second or two.  If it does not,
 * waymacs.el was not loaded or failed; say so where it can be found. */
static int check_emacs_connected(void *data)
{
	if (!ipc_connected() && server.child_pid > 0)
		wlr_log(WLR_ERROR, "Emacs has not connected after 15 s: is waymacs.el loaded "
			"(-l .../waymacs.el -f waymacs-enable)?  An error in the init file, "
			"e.g. in after-init-hook, stops Emacs before it reads its command line");
	return 0;
}

static int handle_sigterm(int signal_number, void *data)
{
	wl_display_terminate(server.display);
	return 0;
}

int main(int argc, char **argv)
{
	const char *startup_command = getenv("WAYMACS_EMACS");
	wlr_log_importance log_level = WLR_INFO;
	int option;
	/* `+': stop at the first non-option, so that `waymacs emacs lispdir'
	 * and `waymacs -- program...' can be told apart */
	while ((option = getopt(argc, argv, "+s:dh")) != -1) {
		switch (option) {
		case 's': startup_command = optarg; break;
		case 'd': log_level = WLR_DEBUG; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}
	bool after_dashes = optind > 1 && strcmp(argv[optind - 1], "--") == 0;
	/* what to run: emacs with waymacs.el, the words after `--', the -s
	 * shell command, or emacs */
	std::vector<std::string> command;
	if (optind < argc && !after_dashes) {
		if (argc - optind != 2) {
			usage(argv[0]);
			return 1;
		}
		std::string lisp_file = std::string(argv[optind + 1]) + "/waymacs.el";
		if (access(lisp_file.c_str(), R_OK) != 0) {
			fprintf(stderr, "waymacs: cannot read %s\n", lisp_file.c_str());
			return 1;
		}
		command = { argv[optind], "-l", lisp_file, "-f", "waymacs-enable" };
	} else if (optind < argc) {
		command.assign(argv + optind, argv + argc);
	} else if (startup_command && *startup_command) {
		command = { "/bin/sh", "-c", startup_command };
	} else {
		command = { "emacs" };
	}
	wlr_log_init(log_level, nullptr);

	server.display = wl_display_create();
	server.event_loop = wl_display_get_event_loop(server.display);
	server.backend = wlr_backend_autocreate(server.event_loop, &server.session);
	if (!server.backend) {
		wlr_log(WLR_ERROR, "failed to create a wlroots backend");
		return 1;
	}
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		wlr_log(WLR_ERROR, "failed to create a renderer");
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);
	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		wlr_log(WLR_ERROR, "failed to create an allocator");
		return 1;
	}

	/* protocols that need nothing more from us */
	server.compositor = wlr_compositor_create(server.display, 6, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_primary_selection_v1_device_manager_create(server.display);
	wlr_data_control_manager_v1_create(server.display);
	wlr_viewporter_create(server.display);
	wlr_single_pixel_buffer_manager_v1_create(server.display);
	wlr_fractional_scale_manager_v1_create(server.display, 1);
#if WLR_VERSION_MINOR >= 19
	wlr_presentation_create(server.display, server.backend, 2);
#else
	wlr_presentation_create(server.display, server.backend);
#endif
	wlr_screencopy_manager_v1_create(server.display);
	wlr_export_dmabuf_manager_v1_create(server.display);
	wlr_server_decoration_manager_set_default_mode(
		wlr_server_decoration_manager_create(server.display),
		WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);

	server.output_layout = wlr_output_layout_create(server.display);
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
	wlr_scene_tree *root = &server.scene->tree;
	server.background_tree = wlr_scene_tree_create(root);
	server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(root);
	server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(root);
	server.frame_tree = wlr_scene_tree_create(root);
	server.tile_tree = wlr_scene_tree_create(root);
	server.floating_tree = wlr_scene_tree_create(root);
	server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(root);
	server.fullscreen_tree = wlr_scene_tree_create(root);
	server.x11_menu_tree = wlr_scene_tree_create(root);
	server.layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(root);

	output_init();
	view_init();
	input_init();
	lock_init();	/* its scene tree goes above everything */
	bar_init();

	const char *socket_name = wl_display_add_socket_auto(server.display);
	if (!socket_name) {
		wlr_log(WLR_ERROR, "failed to open a Wayland socket");
		return 1;
	}
	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start the backend");
		return 1;
	}
	/* children talk to us, not to whatever we may be nested in */
	setenv("WAYLAND_DISPLAY", socket_name, 1);
	xwayland_init();	/* sets DISPLAY */
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("XDG_CURRENT_DESKTOP", "waymacs", 1);
	ipc_init(socket_name);
	wlr_log(WLR_INFO, "running on WAYLAND_DISPLAY=%s", socket_name);

	wl_event_loop_add_signal(server.event_loop, SIGCHLD, handle_sigchld, nullptr);
	wl_event_loop_add_signal(server.event_loop, SIGINT, handle_sigterm, nullptr);
	wl_event_loop_add_signal(server.event_loop, SIGTERM, handle_sigterm, nullptr);
	spawn(command);
	wl_event_source_timer_update(
		wl_event_loop_add_timer(server.event_loop, check_emacs_connected, nullptr), 15000);

	wl_display_run(server.display);
	server.quitting = true;

	if (server.child_pid > 0)
		kill(server.child_pid, SIGTERM);
	ipc_finish();
	xwayland_finish();
	/* Stop listening to the objects that are about to be destroyed
	 * (wlroots 0.19 checks that nothing listens to an object it
	 * destroys).  Windows, outputs and keyboards detach themselves as
	 * they go. */
	server.listeners.clear();
	wl_display_destroy_clients(server.display);
	/* outputs go first: their destroy handlers still use the scene */
	wlr_backend_destroy(server.backend);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.xcursor_manager);
	wlr_cursor_destroy(server.cursor);
	wlr_keyboard_finish(&server.inject_keyboard);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wl_display_destroy(server.display);
	return 0;
}
