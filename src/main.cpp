/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* main.cpp - setup, protocol globals, the startup command, shutdown. */
#include "waymacs.h"
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

struct waymacs server;

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [-d] [-s command | -- program [argument...]]\n"
		"  -s command  run the shell COMMAND\n"
		"  -- program  run PROGRAM with its arguments, without a shell\n"
		"              (default: $WAYMACS_EMACS or \"emacs\");\n"
		"              the session ends when it exits\n"
		"  -d          debug logging\n"
		"example: %s -- emacs -l /path/to/waymacs/lisp/waymacs.el -f waymacs-enable\n",
		program, program);
}

/* Run COMMAND_ARGV (a NULL-terminated argument vector). */
static void spawn(char *const command_argv[])
{
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork");
		return;
	}
	if (pid == 0) {
		/* wl_event_loop_add_signal() blocked these in the parent */
		sigset_t signals;
		sigemptyset(&signals);
		sigprocmask(SIG_SETMASK, &signals, NULL);
		setsid();
		execvp(command_argv[0], command_argv);
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

/* Stop listening to the objects that are about to be destroyed (wlroots
 * 0.19 checks that nothing listens to an object it destroys).  Windows,
 * outputs and keyboards detach themselves as they go. */
static void detach_listeners(void)
{
	stop_listening(&server.new_output, &server.output_layout_change, &server.new_toplevel,
		       &server.new_popup, &server.new_layer_surface, &server.new_decoration,
		       &server.request_activate, &server.new_input, &server.new_virtual_keyboard,
		       &server.new_virtual_pointer, &server.request_cursor_shape,
		       &server.cursor_motion, &server.cursor_motion_absolute,
		       &server.cursor_button, &server.cursor_axis, &server.cursor_frame,
		       &server.request_cursor, &server.request_selection,
		       &server.request_primary_selection, &server.request_start_drag,
		       &server.new_lock, &server.lock_new_surface, &server.lock_unlock,
		       &server.lock_destroy, &server.lock_manager_destroy);
}

static int handle_sigterm(int signal_number, void *data)
{
	wl_display_terminate(server.display);
	return 0;
}

int main(int argc, char **argv)
{
	const char *startup_command = getenv("WAYMACS_EMACS");
	enum wlr_log_importance log_level = WLR_INFO;
	int option;
	while ((option = getopt(argc, argv, "s:dh")) != -1) {
		switch (option) {
		case 's': startup_command = optarg; break;
		case 'd': log_level = WLR_DEBUG; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 1;
		}
	}
	/* what to run: the words after `--', the -s shell command, or emacs */
	static char shell[] = "/bin/sh", shell_option[] = "-c", emacs[] = "emacs";
	char *shell_argv[] = { shell, shell_option, NULL, NULL };
	char *emacs_argv[] = { emacs, NULL };
	char *const *command_argv;
	if (optind < argc) {
		command_argv = &argv[optind];
	} else if (startup_command && *startup_command) {
		shell_argv[2] = (char *)startup_command;
		command_argv = shell_argv;
	} else {
		command_argv = emacs_argv;
	}
	wlr_log_init(log_level, NULL);

	wl_list_init(&server.outputs);
	wl_list_init(&server.disabled_outputs);
	wl_list_init(&server.views);
	wl_list_init(&server.keyboards);
	server.next_view_id = 1;
	server.ipc_fd = server.ipc_listen_fd = -1;
	server.repeat_rate = 25;
	server.repeat_delay = 600;

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
	struct wlr_scene_tree *root = &server.scene->tree;
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

	wl_event_loop_add_signal(server.event_loop, SIGCHLD, handle_sigchld, NULL);
	wl_event_loop_add_signal(server.event_loop, SIGINT, handle_sigterm, NULL);
	wl_event_loop_add_signal(server.event_loop, SIGTERM, handle_sigterm, NULL);
	spawn(command_argv);
	wl_event_source_timer_update(
		wl_event_loop_add_timer(server.event_loop, check_emacs_connected, NULL), 15000);

	wl_display_run(server.display);
	server.quitting = true;

	if (server.child_pid > 0)
		kill(server.child_pid, SIGTERM);
	ipc_finish();
	xwayland_finish();
	detach_listeners();
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
