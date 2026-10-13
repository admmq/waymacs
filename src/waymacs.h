/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* waymacs - Emacs as a Wayland window manager.
 *
 * The compositor is deliberately small: it shows Emacs frames full-output,
 * places application windows where Emacs says (one Emacs window = one
 * rectangle), routes keyboard focus, and hands a few "prefix" keys to
 * Emacs.  All window-management policy lives in waymacs.el.
 *
 * The code is C written as C++ (no classes, no STL, no exceptions).
 */
#ifndef WAYMACS_H
#define WAYMACS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <pixman.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>

/* wlroots 0.18 and 0.19 are supported; WLR_VERSION_MINOR tells them
 * apart where their APIs differ. */
#include <wlr/version.h>

/* wlroots headers are C99: `[static N]' array parameters are not C++,
 * layer-shell names a field `namespace' and Xwayland one `class'.  System
 * headers above are already included, so the macros only touch the
 * wlroots declarations. */
extern "C" {
#define static
#define namespace namespace_
#define class class_
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#undef class
#undef namespace
#undef static
}

/* Call HANDLER on SIGNAL.  HANDLER is a function or, for one-liners, a
 * lambda written in place (the ... lets its body contain commas):
 *   LISTEN(&surface->events.map, &view->map, [](struct wl_listener *listener, void *data) {
 *           view_map(OWNER(listener, struct view, map));
 *   });
 */
#define LISTEN(signal, listener, ...) \
	do { (listener)->notify = (__VA_ARGS__); wl_signal_add((signal), (listener)); } while (0)

/* the TYPE whose wl_listener field MEMBER is LISTENER */
#define OWNER(listener, type, member) wl_container_of(listener, (type *)NULL, member)

/* Stop the listeners that listen; others (never added, or stopped
 * already) are left alone, so this is safe to call more than once. */
template <typename... Listeners>
static inline void stop_listening(Listeners *... listeners)
{
	struct wl_listener *all[] = { listeners... };
	for (struct wl_listener *listener : all) {
		if (!listener->link.next)	/* zeroed: never added */
			continue;
		wl_list_remove(&listener->link);
		wl_list_init(&listener->link);
	}
}

/* modifiers that take part in key bindings (not Caps Lock, Num Lock, AltGr) */
#define MODIFIER_MASK (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | \
		       WLR_MODIFIER_MOD3 | WLR_MODIFIER_LOGO)
#define MAX_KEYS 256
#define MAX_SIMULATED_KEYS 8

/* Every scene tree we own points (node.data) at a struct starting with
 * one of these, so the pointer code can tell what it hit. */
enum owner_type { OWNER_VIEW = 1, OWNER_LAYER };

struct output {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *background;
	struct wlr_scene_rect *lock_background;	/* covers it while locked */
	struct wlr_box box;		/* whole output, layout coordinates */
	struct wlr_box usable_box;	/* box minus layer-shell exclusive zones */
	struct wl_list layers[4];	/* struct layer, one list per layer-shell layer */
	struct view *active_frame;	/* the Emacs frame shown on this output */
	bool enabled;			/* off: in server.disabled_outputs */
	struct wl_listener render, request_state, destroy;
};

/* A toplevel window: either an Emacs frame or an application.  It is a
 * Wayland (xdg-shell) window or an X11 one (Xwayland, never a frame). */
struct view {
	int type;			/* OWNER_VIEW */
	struct wl_list link;
	uint32_t id;
	pid_t pid;
	struct wlr_xdg_toplevel *toplevel;	/* one of these two */
	struct wlr_xwayland_surface *xwayland_surface;
	struct wlr_scene_tree *surface_tree;	/* X11: the surface, while mapped */
	struct wlr_scene_tree *scene_tree;	/* origin = window geometry origin */
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	bool mapped;
	bool is_frame;			/* the toplevel belongs to the Emacs process */

	/* Emacs frames */
	char tag[64];			/* "waymacs-frame-N" title used to pair it */
	struct output *output;
	uint64_t last_focused;		/* focus order, to pick a frame to show */
	int workspace;			/* its workspace number (from Emacs), 0 if none */
	int minibuffer_top;		/* frame-relative top of its minibuffer,
					   0 if unknown (from Emacs) */

	/* applications */
	bool announced;			/* Emacs knows about it */
	bool unmanaged;			/* mapped while no Emacs was connected */
	bool in_layout;			/* shown in an Emacs window */
	bool floating, fullscreen, char_mode;
	struct view *frame;		/* the Emacs frame it is shown on */
	struct wlr_box tile_box;	/* frame-relative, from the layout */
	struct wlr_box floating_box;	/* layout coordinates */
	int configured_x, configured_y;		/* last position sent (X11) */
	int configured_width, configured_height;	/* last size sent */

	struct wl_listener map, unmap, commit, destroy, set_title, set_app_id;
	struct wl_listener request_fullscreen, request_maximize, request_minimize;
	struct wl_listener request_move, request_resize;
	struct wl_listener associate, dissociate, request_configure, request_activate;
	struct wl_listener set_override_redirect;
	struct wl_listener decoration_request_mode, decoration_destroy;

	/* taskbars (bar.cpp) */
	struct wlr_foreign_toplevel_handle_v1 *taskbar_handle;
	struct wlr_output *taskbar_output;
	struct wl_listener taskbar_activate, taskbar_close, taskbar_fullscreen, taskbar_destroy;
};

/* A layer-shell surface: bar, wallpaper, launcher, notification... */
struct layer {
	int type;			/* OWNER_LAYER */
	struct wl_list link;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct output *output;
	bool mapped;
	struct wl_listener unmap, commit, destroy, new_popup;
};

struct keyboard {
	struct wl_list link;
	struct wlr_keyboard *wlr_keyboard;
	bool is_virtual;
	uint32_t swallowed_keycodes[16];	/* presses eaten; eat their releases */
	uint32_t held_physical_keycode;		/* a simulation key held down... */
	uint32_t held_injected_keycode;		/* ...and the key we pressed for it */
	struct wl_listener modifiers, key, destroy;
};

struct key_spec {
	uint32_t modifiers;		/* WLR_MODIFIER_* bits */
	xkb_keysym_t keysym;
};

/* a key typed in an application as other keys (line mode only) */
struct simulation_key {
	struct key_spec from;
	int count;
	struct key_spec to[MAX_SIMULATED_KEYS];
};

enum cursor_mode { CURSOR_PASSTHROUGH, CURSOR_MOVE, CURSOR_RESIZE };

struct waymacs {
	struct wl_display *display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_output_layout *output_layout;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	/* stacking order, bottom to top */
	struct wlr_scene_tree *background_tree, *layer_trees[4];
	struct wlr_scene_tree *frame_tree, *tile_tree, *floating_tree, *fullscreen_tree;
	struct wlr_scene_tree *x11_menu_tree;	/* X11 menus, tooltips */

	struct wl_list outputs, views, keyboards;
	struct wl_list disabled_outputs;	/* turned off (laptop lid closed) */
	bool lid_closed;
	bool quitting;			/* the session is being torn down */
	struct wlr_compositor *compositor;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_xwayland *xwayland;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_seat *seat;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_manager;
	/* types simulated keys when the active keyboard's keymap cannot
	 * (virtual keyboards upload minimal keymaps) */
	struct wlr_keyboard inject_keyboard;

	/* Keyboard focus.  `focused_view' is what Emacs (or a click) asked
	 * for; a layer surface wanting the keyboard takes precedence. */
	struct view *focused_view;
	bool emacs_wants_frame;		/* Emacs asked for its own frame: it is
					   shown over fullscreen applications */
	struct layer *focused_layer;
	uint64_t focus_counter;

	/* screen locker: `locked' stays true if the locker dies */
	bool locked;
	struct wlr_session_lock_v1 *lock;
	struct wlr_scene_tree *lock_tree;

	/* status bars (bar.cpp) */
	struct wl_list output_statuses, seat_statuses;
	uint32_t unshown_workspace_tags;	/* workspaces never shown yet */
	struct wlr_foreign_toplevel_manager_v1 *taskbar_manager;

	enum cursor_mode cursor_mode;
	struct view *grabbed_view;
	double grab_cursor_x, grab_cursor_y;
	struct wlr_box grab_box;
	uint32_t grab_edges;

	pid_t emacs_pid, child_pid;
	uint32_t next_view_id;

	struct key_spec global_keys[MAX_KEYS], prefix_keys[MAX_KEYS];
	int global_key_count, prefix_key_count;
	struct simulation_key simulation_keys[MAX_KEYS];
	int simulation_key_count;
	char xkb_layout[128], xkb_variant[128], xkb_options[256];
	int repeat_rate, repeat_delay;

	int ipc_listen_fd, ipc_fd;
	struct wl_event_source *ipc_listen_source, *ipc_source;
	char *ipc_input, *ipc_output;
	size_t ipc_input_length, ipc_input_capacity;
	size_t ipc_output_length, ipc_output_capacity;
	char socket_path[256];

	struct wl_listener new_output, output_layout_change, new_toplevel, new_popup;
	struct wl_listener new_layer_surface, new_decoration, request_activate, new_input;
	struct wl_listener new_virtual_keyboard, new_virtual_pointer, request_cursor_shape;
	struct wl_listener cursor_motion, cursor_motion_absolute, cursor_button;
	struct wl_listener cursor_axis, cursor_frame;
	struct wl_listener request_cursor, request_selection, request_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener new_lock, lock_new_surface, lock_unlock, lock_destroy;
	struct wl_listener lock_manager_destroy;
	struct wl_listener xwayland_ready, new_xwayland_surface;
};

extern struct waymacs server;

/* ipc.cpp */
void ipc_init(const char *display_name);
void ipc_finish(void);
bool ipc_connected(void);
void ipc_send(const char *format, ...) __attribute__((format(printf, 1, 2)));
const char *lisp_string(const char *text);
bool parse_key_spec(const char *text, struct key_spec *key, bool lowercase);

/* view.cpp */
void view_init(void);
struct view *view_create(pid_t pid);
void view_map(struct view *view);
void view_unmap(struct view *view);
void view_destroy(struct view *view);
void view_send_update(struct view *view);
void view_handle_set_title(struct wl_listener *listener, void *data);
void view_handle_set_app_id(struct wl_listener *listener, void *data);
void view_handle_request_move(struct wl_listener *listener, void *data);
struct wlr_surface *view_surface(struct view *view);
const char *view_title(struct view *view);
const char *view_app_id(struct view *view);
void keyboard_enter_now(struct wlr_surface *surface);
struct view *view_by_id(uint32_t id);
bool frame_is_visible(struct view *frame);
struct output *focused_output(void);
void arrange(void);
void focus_view(struct view *view, bool requested_by_emacs);
void focus_frame_for_key(struct view *frame, uint32_t keycode);
void apply_keyboard_focus(uint32_t excluded_keycode);
void focus_fallback(void);
void emacs_hello(pid_t pid);
void view_set_fullscreen(struct view *view, bool fullscreen);
void view_set_floating(struct view *view, bool floating);
void view_close(struct view *view, bool kill_client);
void view_set_frame(struct view *app, struct view *frame);
void view_request_activate(struct view *view);
void frame_move_to_output(struct view *frame, struct output *output);
void frame_set_output(struct view *frame, struct output *output);
void layout_begin(void);
void layout_add(uint32_t frame_id, uint32_t app_id, int x, int y, int width, int height);
void layout_end(void);
void popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree);

/* input.cpp */
void input_init(void);
void keyboards_reconfigure(void);
void inject_keys(const struct key_spec *keys, int count);
void update_pointer_focus(void);
void begin_grab(struct view *view, enum cursor_mode mode, uint32_t edges);

/* bar.cpp */
void bar_init(void);
void bar_update(void);
void bar_hello(void);
void bar_view_mapped(struct view *app);
void bar_view_unmapped(struct view *app);
void bar_view_updated(struct view *app);
void bar_output_destroyed(struct wlr_output *wlr_output);

/* lock.cpp */
void lock_init(void);
void lock_arrange(void);
struct wlr_surface *lock_keyboard_target(void);
bool is_lock_surface(struct wlr_surface *surface);

/* xwayland.cpp */
void xwayland_init(void);
void xwayland_finish(void);
bool xwayland_wants_to_float(struct wlr_xwayland_surface *surface);

/* output.cpp */
void output_init(void);
void layers_arrange(struct output *output);
void outputs_announce(void);
struct output *output_by_name(const char *name, bool enabled_only);
void output_set_enabled(struct output *output, bool enabled);

#endif
