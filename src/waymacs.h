/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* waymacs - Emacs as a Wayland window manager.
 *
 * The compositor is deliberately small: it shows Emacs frames full-output,
 * places application windows where Emacs says (one Emacs window = one
 * rectangle), routes keyboard focus, and hands a few "prefix" keys to
 * Emacs.  All window-management policy lives in waymacs.el.
 *
 * C++17 with the standard library, without exceptions or RTTI (an
 * allocation failure aborts).  Objects tied to a wlroots object delete
 * themselves when it is destroyed; their listeners go with them.
 */
#ifndef WAYMACS_H
#define WAYMACS_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>
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

/* Calls a handler when a wl_signal is emitted, until it is stopped or
 * destroyed.  A handler may destroy its own listener (destroy handlers
 * delete the object holding it): nothing is touched after the call. */
class Listener {
public:
	using Handler = std::function<void(void *data)>;
	Listener() { wl_list_init(&hook.listener.link); }
	~Listener() { stop(); }
	Listener(const Listener &) = delete;
	Listener &operator=(const Listener &) = delete;

	void listen(wl_signal *signal, Handler new_handler)
	{
		stop();
		handler = std::move(new_handler);
		wl_signal_add(signal, &hook.listener);
	}
	/* safe to call more than once */
	void stop()
	{
		wl_list_remove(&hook.listener.link);
		wl_list_init(&hook.listener.link);
	}

private:
	struct Hook {
		wl_listener listener;
		Listener *owner;
	} hook = { { {}, notify }, this };
	Handler handler;

	static void notify(wl_listener *listener, void *data)
	{
		reinterpret_cast<Hook *>(listener)->owner->handler(data);
	}
};

template <typename T>
void erase(std::vector<T> &vector, const T &value)
{
	vector.erase(std::remove(vector.begin(), vector.end(), value), vector.end());
}

/* modifiers that take part in key bindings (not Caps Lock, Num Lock, AltGr) */
constexpr uint32_t MODIFIER_MASK = WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
	WLR_MODIFIER_MOD3 | WLR_MODIFIER_LOGO;
/* "no key excluded" for apply_keyboard_focus() */
constexpr uint32_t NO_KEY = UINT32_MAX;

struct KeySpec {
	uint32_t modifiers;		/* WLR_MODIFIER_* bits */
	xkb_keysym_t keysym;
};

/* a key typed in an application as other keys (line mode only) */
struct SimulationKey {
	KeySpec from;
	std::vector<KeySpec> to;
};

/* Settings from Emacs (waymacs-output-settings, waymacs-pointer-settings):
 * the NAME=VALUE words of the command that sent them */
struct Settings {
	std::map<std::string, std::string> values;
	const char *get(const char *name) const	/* null if not set */
	{
		auto found = values.find(name);
		return found == values.end() ? nullptr : found->second.c_str();
	}
};

enum class CursorMode { passthrough, move, resize };

/* Every scene tree we own points (node.data) at one of these, so the
 * pointer code can tell what it hit. */
struct SceneOwner {
	enum Kind { VIEW, LAYER } kind;
};

struct Output;
struct Keyboard;

/* A toplevel window: either an Emacs frame or an application.  It is a
 * Wayland (xdg-shell) window or an X11 one (Xwayland, never a frame); the
 * virtual functions are what differs between the two. */
struct View : SceneOwner {
	explicit View(pid_t pid);
	virtual ~View();

	virtual wlr_surface *surface() = 0;
	virtual const char *title_or_null() = 0;
	virtual const char *app_id_or_null() = 0;	/* the Wayland app id, or the X11 class */
	virtual wlr_box geometry() = 0;
	/* Give the window its size.  X11 windows also learn their position
	 * (layout coordinates): their menus are placed with it. */
	virtual void configure(int x, int y, int width, int height) = 0;
	virtual void set_activated(bool activated) = 0;
	virtual void set_fullscreen_state(bool fullscreen) = 0;
	virtual View *parent() = 0;	/* the window it is a dialog of */
	/* Emacs decides from these whether the window floats */
	virtual bool has_fixed_size() = 0;	/* it cannot be resized */
	virtual bool is_modal() { return false; }	/* X11 windows only */
	virtual const char *window_type() { return nullptr; }	/* X11: "dialog"... */
	virtual void close(bool kill_client) = 0;
	/* the node showing the window itself, without its popups */
	virtual wlr_scene_node *surface_node() = 0;
	virtual bool is_x11() = 0;
	/* Emacs turned out to own this window (see emacs_hello) */
	virtual void became_frame() {}

	const char *title() { const char *title = title_or_null(); return title ? title : ""; }
	const char *app_id() { const char *app_id = app_id_or_null(); return app_id ? app_id : ""; }

	uint32_t id;
	pid_t pid;
	wlr_scene_tree *scene_tree = nullptr;	/* origin = window geometry origin */
	bool mapped = false;
	bool is_frame = false;		/* the toplevel belongs to the Emacs process */

	/* Emacs frames */
	std::string tag;		/* "waymacs-frame-N" title used to pair it */
	Output *output = nullptr;
	uint64_t last_focused = 0;	/* focus order, to pick a frame to show */
	int workspace = 0;		/* its workspace number (from Emacs), 0 if none */
	int minibuffer_top = 0;		/* frame-relative top of its minibuffer,
					   0 if unknown (from Emacs) */

	/* applications */
	bool announced = false;		/* Emacs knows about it */
	bool unmanaged = false;		/* mapped while no Emacs was connected */
	bool in_layout = false;		/* shown in an Emacs window */
	bool floating = false, fullscreen = false, char_mode = false;
	View *frame = nullptr;		/* the Emacs frame it is shown on */
	wlr_box tile_box = {};		/* frame-relative, from the layout */
	wlr_box floating_box = {};	/* layout coordinates */
	int configured_x = INT32_MIN, configured_y = INT32_MIN;	/* last position sent (X11) */
	int configured_width = 0, configured_height = 0;	/* last size sent */

	/* taskbars (bar.cpp) */
	wlr_foreign_toplevel_handle_v1 *taskbar_handle = nullptr;
	struct wlr_output *taskbar_output = nullptr;
	Listener taskbar_activate, taskbar_close, taskbar_fullscreen, taskbar_destroy;
};

/* the Emacs frame (workspace) VIEW is on: VIEW itself for a frame */
inline View *frame_of(View *view)
{
	return view && !view->is_frame ? view->frame : view;
}

/* A layer-shell surface: bar, wallpaper, launcher, notification... */
struct Layer : SceneOwner {
	Layer(wlr_layer_surface_v1 *layer_surface, Output *output);
	~Layer();
	bool keyboard_interactivity_is(zwlr_layer_surface_v1_keyboard_interactivity mode)
	{
		return layer_surface->current.keyboard_interactive == mode;
	}
	wlr_layer_surface_v1 *layer_surface;
	wlr_scene_layer_surface_v1 *scene_layer;
	Output *output;
	bool mapped = false;
	Listener unmap, commit, destroy, new_popup;
};

struct Output {
	explicit Output(struct wlr_output *wlr_output);
	~Output();
	struct wlr_output *wlr_output;
	wlr_scene_output *scene_output = nullptr;
	wlr_scene_rect *background, *lock_background;	/* the latter covers it while locked */
	wlr_box box = {};		/* whole output, layout coordinates */
	wlr_box usable_box = {};	/* box minus layer-shell exclusive zones */
	std::array<std::vector<Layer *>, 4> layers;	/* one list per layer-shell layer */
	View *active_frame = nullptr;	/* the Emacs frame shown on this output */
	bool enabled = false;		/* off: in server.disabled_outputs */
	Listener render, request_state, destroy;
};

struct Server {
	wl_display *display;
	wl_event_loop *event_loop;
	wlr_backend *backend;
	wlr_session *session = nullptr;
	wlr_renderer *renderer;
	wlr_allocator *allocator;
	wlr_output_layout *output_layout;
	wlr_scene *scene;
	wlr_scene_output_layout *scene_layout;

	/* stacking order, bottom to top */
	wlr_scene_tree *background_tree, *layer_trees[4];
	wlr_scene_tree *frame_tree, *tile_tree, *floating_tree, *fullscreen_tree;
	wlr_scene_tree *x11_menu_tree;	/* X11 menus, tooltips */
	wlr_scene_tree *lock_tree;

	std::vector<Output *> outputs;
	std::vector<Output *> disabled_outputs;	/* turned off (laptop lid closed) */
	std::vector<View *> views;
	std::vector<Keyboard *> keyboards;
	bool lid_closed = false;
	bool quitting = false;		/* the session is being torn down */
	wlr_compositor *compositor;
	wlr_seat *seat;
	wlr_cursor *cursor;
	wlr_xcursor_manager *xcursor_manager;
	/* types simulated keys when the active keyboard's keymap cannot
	 * (virtual keyboards upload minimal keymaps) */
	struct wlr_keyboard inject_keyboard;

	/* Keyboard focus.  `focused_view' is what Emacs (or a click) asked
	 * for; a layer surface wanting the keyboard takes precedence. */
	View *focused_view = nullptr;
	bool emacs_wants_frame = false;	/* Emacs asked for its own frame: it is
					   shown over fullscreen applications */
	Layer *focused_layer = nullptr;
	uint64_t focus_counter = 0;

	bool locked = false;		/* stays true if the screen locker dies */
	uint32_t unshown_workspace_tags = 0;	/* workspaces never shown yet */
	wlr_foreign_toplevel_manager_v1 *taskbar_manager;

	pid_t emacs_pid = 0, child_pid = 0;
	uint32_t next_view_id = 1;

	std::vector<KeySpec> global_keys, prefix_keys;
	std::vector<SimulationKey> simulation_keys;
	std::string xkb_layout, xkb_variant, xkb_options;
	int repeat_rate = 25, repeat_delay = 600;
	/* waymacs-pointer-settings, and waymacs-output-settings by monitor
	 * name (kept for a monitor plugged in again) */
	Settings touchpad_settings, mouse_settings;
	std::map<std::string, Settings> output_settings;

	/* Listen to SIGNAL of an object that outlives the session's clients;
	 * main() stops these before tearing the compositor down. */
	void listen(wl_signal *signal, Listener::Handler handler);
	std::vector<std::unique_ptr<Listener>> listeners;
};

extern Server server;

/* ipc.cpp */
void ipc_init(const char *display_name);
void ipc_finish();
bool ipc_connected();
void ipc_send(const char *format, ...) __attribute__((format(printf, 1, 2)));
const char *lisp_string(const char *text);

/* view.cpp */
void view_init();
void view_map(View *view);
void view_unmap(View *view);
void view_send_update(View *view);
void view_title_changed(View *view);
void keyboard_enter_now(wlr_surface *surface);
View *view_by_id(uint32_t id);
bool frame_is_visible(View *frame);
Output *focused_output();
void arrange();
void focus_view(View *view, bool requested_by_emacs);
void focus_frame_for_key(View *frame, uint32_t keycode);
void apply_keyboard_focus(uint32_t excluded_keycode = NO_KEY);
void focus_fallback();
void emacs_hello(pid_t pid);
void view_set_fullscreen(View *view, bool fullscreen);
void view_set_floating(View *view, bool floating);
void view_set_frame(View *app, View *frame);
void view_request_activate(View *view);
void frame_move_to_output(View *frame, Output *output);
void frame_set_output(View *frame, Output *output);
void frame_assign_output(View *frame, Output *output);
void ask_frame_for_empty_output(Output *output);
void layout_begin();
void layout_add(uint32_t frame_id, uint32_t app_id, int x, int y, int width, int height);
void layout_end();
void popup_create(wlr_xdg_popup *xdg_popup, wlr_scene_tree *parent_tree);

/* input.cpp */
void input_init();
void keyboards_reconfigure();
void pointers_reconfigure();
void inject_keys(const std::vector<KeySpec> &keys);
void update_pointer_focus();
void begin_grab(View *view, CursorMode mode, uint32_t edges);	/* floating views only */
void end_grab(View *view = nullptr);	/* any grab, or only VIEW's */

/* bar.cpp */
void bar_init();
void bar_update();
void bar_hello();
void bar_view_mapped(View *app);
void bar_view_unmapped(View *app);
void bar_view_updated(View *app);
void bar_output_destroyed(struct wlr_output *wlr_output);

/* lock.cpp */
void lock_init();
void lock_arrange();
wlr_surface *lock_keyboard_target();
bool is_lock_surface(wlr_surface *surface);

/* xwayland.cpp */
void xwayland_init();
void xwayland_finish();

/* output.cpp */
void output_init();
void outputs_announce();
Output *output_by_name(const char *name, bool enabled_only);
void output_set_enabled(Output *output, bool enabled);
void output_configure(const std::string &name, Settings settings);

#endif
