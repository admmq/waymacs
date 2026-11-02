/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* xwayland.cpp - X11 applications, through Xwayland.
 *
 * Ordinary X11 windows become views like Wayland ones: Emacs gets a
 * buffer for each.  Override-redirect windows (menus, tooltips, drop-down
 * lists) place themselves; we only show them, above everything but the
 * lock screen.  Xwayland is started when the first X11 client connects. */
#include "waymacs.h"
#include <signal.h>

static wlr_xwayland *xwayland;
static Listener xwayland_ready, new_xwayland_surface;

/* window types (_NET_WM_WINDOW_TYPE) that float like dialogs */
static const char *const floating_type_names[] = {
	"_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
	"_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_SPLASH",
};
static std::array<xcb_atom_t, std::size(floating_type_names)> floating_types;

/* ---- ordinary windows: views ---- */

struct X11View : View {
	explicit X11View(wlr_xwayland_surface *surface);
	~X11View() override;
	void handle_map();
	void handle_unmap();
	void handle_request_configure(wlr_xwayland_surface_configure_event *event);

	wlr_surface *surface() override { return xwayland_surface->surface; }
	const char *title_or_null() override { return xwayland_surface->title; }
	const char *app_id_or_null() override { return xwayland_surface->class_; }
	wlr_box geometry() override { return { 0, 0, xwayland_surface->width, xwayland_surface->height }; }
	wlr_scene_node *surface_node() override { return surface_tree ? &surface_tree->node : nullptr; }
	bool is_x11() override { return true; }

	void configure(int x, int y, int width, int height) override
	{
		if (x == configured_x && y == configured_y &&
		    width == configured_width && height == configured_height)
			return;
		configured_x = x;
		configured_y = y;
		configured_width = width;
		configured_height = height;
		wlr_xwayland_surface_configure(xwayland_surface, x, y, width, height);
	}

	void set_activated(bool activated) override
	{
		wlr_xwayland_surface_activate(xwayland_surface, activated);
		if (activated)
			wlr_xwayland_surface_restack(xwayland_surface, nullptr, XCB_STACK_MODE_ABOVE);
	}

	void set_fullscreen_state(bool fullscreen) override
	{
		wlr_xwayland_surface_set_fullscreen(xwayland_surface, fullscreen);
	}

	View *parent() override
	{
		return xwayland_surface->parent ?
			static_cast<View *>(xwayland_surface->parent->data) : nullptr;
	}

	/* dialogs, splash screens, and windows that cannot be resized */
	bool wants_to_float() override
	{
		if (xwayland_surface->modal || xwayland_surface->parent)
			return true;
		xcb_atom_t *types = xwayland_surface->window_type;
		if (std::any_of(types, types + xwayland_surface->window_type_len, [](xcb_atom_t type) {
			return type && std::count(floating_types.begin(), floating_types.end(), type);
		}))
			return true;
		xcb_size_hints_t *hints = xwayland_surface->size_hints;
		return hints && hints->min_width > 0 && hints->min_height > 0 &&
		       hints->min_width == hints->max_width && hints->min_height == hints->max_height;
	}

	void close(bool kill_client) override
	{
		/* all X11 clients share Xwayland's Wayland connection */
		if (kill_client && xwayland_surface->pid > 0)
			kill(xwayland_surface->pid, SIGKILL);
		else
			wlr_xwayland_surface_close(xwayland_surface);
	}

	wlr_xwayland_surface *xwayland_surface;
	wlr_scene_tree *surface_tree = nullptr;		/* the surface, while mapped */
	Listener associate, dissociate, map, unmap, destroy, request_configure, request_activate;
	Listener request_fullscreen, request_move, request_resize, set_title, set_class;
	Listener set_override_redirect;
};

/* override-redirect windows */
struct X11Popup {
	explicit X11Popup(wlr_xwayland_surface *surface);
	void handle_map();
	void handle_unmap();

	wlr_xwayland_surface *xwayland_surface;
	wlr_scene_tree *surface_tree = nullptr;		/* while mapped */
	Listener associate, dissociate, map, unmap, request_configure;
	Listener set_geometry, set_override_redirect, destroy;
};

/* An X11 window's Wayland surface comes (associate) and goes
 * (dissociate); WINDOW's handle_map() and handle_unmap() follow it. */
template <typename Window>
static void follow_surface(Window *window)
{
	wlr_xwayland_surface *surface = window->xwayland_surface;
	auto associate = [window, surface](void *) {
		window->map.listen(&surface->surface->events.map,
				   [window](void *) { window->handle_map(); });
		window->unmap.listen(&surface->surface->events.unmap,
				     [window](void *) { window->handle_unmap(); });
	};
	window->associate.listen(&surface->events.associate, associate);
	window->dissociate.listen(&surface->events.dissociate, [window](void *) {
		window->map.stop();
		window->unmap.stop();
	});
	if (surface->surface) {
		associate(nullptr);
		if (surface->surface->mapped)
			window->handle_map();
	}
}

X11View::X11View(wlr_xwayland_surface *surface) : View(surface->pid), xwayland_surface(surface)
{
	scene_tree = wlr_scene_tree_create(server.tile_tree);
	scene_tree->node.data = static_cast<SceneOwner *>(this);
	wlr_scene_node_set_enabled(&scene_tree->node, false);
	surface->data = static_cast<View *>(this);

	destroy.listen(&surface->events.destroy, [this](void *) { delete this; });
	request_configure.listen(&surface->events.request_configure, [this](void *data) {
		handle_request_configure(static_cast<wlr_xwayland_surface_configure_event *>(data));
	});
	request_activate.listen(&surface->events.request_activate, [this](void *) {
		view_request_activate(this);
	});
	request_fullscreen.listen(&surface->events.request_fullscreen, [this](void *) {
		if (mapped)
			view_set_fullscreen(this, xwayland_surface->fullscreen);
	});
	request_move.listen(&surface->events.request_move, [this](void *) {
		begin_grab(this, CursorMode::move, 0);
	});
	request_resize.listen(&surface->events.request_resize, [this](void *data) {
		begin_grab(this, CursorMode::resize,
			   static_cast<wlr_xwayland_resize_event *>(data)->edges);
	});
	set_title.listen(&surface->events.set_title, [this](void *) { view_title_changed(this); });
	set_class.listen(&surface->events.set_class, [this](void *) { view_send_update(this); });
	/* An unmapped window may turn into an override-redirect one, or back. */
	set_override_redirect.listen(&surface->events.set_override_redirect, [this](void *) {
		wlr_xwayland_surface *surface = xwayland_surface;
		if (mapped)
			handle_unmap();
		delete this;
		new X11Popup(surface);
	});
	follow_surface(this);
}

X11View::~X11View()
{
	xwayland_surface->data = nullptr;
	wlr_scene_node_destroy(&scene_tree->node);
}

void X11View::handle_map()
{
	surface_tree = wlr_scene_subsurface_tree_create(scene_tree, xwayland_surface->surface);
	pid = xwayland_surface->pid;
	fullscreen = xwayland_surface->fullscreen;
	configured_x = configured_y = INT32_MIN;	/* configure it again */
	view_map(this);
}

void X11View::handle_unmap()
{
	wlr_scene_node_destroy(&surface_tree->node);
	surface_tree = nullptr;
	view_unmap(this);
}

/* X11 clients size and move their windows themselves.  Before mapping
 * they get what they ask for; afterwards, floating windows may resize,
 * the others are told their place again. */
void X11View::handle_request_configure(wlr_xwayland_surface_configure_event *event)
{
	if (!mapped) {
		wlr_xwayland_surface_configure(xwayland_surface, event->x, event->y,
					       event->width, event->height);
		return;
	}
	if (floating && !fullscreen) {
		floating_box.width = event->width;
		floating_box.height = event->height;
	}
	configured_x = configured_y = INT32_MIN;
	arrange();
	if (configured_x == INT32_MIN)	/* not shown: answer anyway */
		wlr_xwayland_surface_configure(xwayland_surface, xwayland_surface->x,
					       xwayland_surface->y, xwayland_surface->width,
					       xwayland_surface->height);
}

/* ---- override-redirect windows ---- */

X11Popup::X11Popup(wlr_xwayland_surface *surface) : xwayland_surface(surface)
{
	request_configure.listen(&surface->events.request_configure, [this](void *data) {
		auto *event = static_cast<wlr_xwayland_surface_configure_event *>(data);
		wlr_xwayland_surface_configure(xwayland_surface, event->x, event->y,
					       event->width, event->height);
	});
	set_geometry.listen(&surface->events.set_geometry, [this](void *) {
		if (surface_tree)
			wlr_scene_node_set_position(&surface_tree->node, xwayland_surface->x,
						    xwayland_surface->y);
	});
	set_override_redirect.listen(&surface->events.set_override_redirect, [this](void *) {
		wlr_xwayland_surface *surface = xwayland_surface;
		if (surface_tree)
			handle_unmap();
		delete this;
		new X11View(surface);
	});
	destroy.listen(&surface->events.destroy, [this](void *) { delete this; });
	follow_surface(this);
}

void X11Popup::handle_map()
{
	surface_tree = wlr_scene_subsurface_tree_create(server.x11_menu_tree,
							xwayland_surface->surface);
	wlr_scene_node_set_position(&surface_tree->node, xwayland_surface->x, xwayland_surface->y);
#if WLR_VERSION_MINOR >= 19
	bool wants_focus = wlr_xwayland_surface_override_redirect_wants_focus(xwayland_surface);
#else
	bool wants_focus = wlr_xwayland_or_surface_wants_focus(xwayland_surface);
#endif
	if (!server.locked && wants_focus)
		keyboard_enter_now(xwayland_surface->surface);
}

void X11Popup::handle_unmap()
{
	wlr_scene_node_destroy(&surface_tree->node);
	surface_tree = nullptr;
	/* the keyboard goes back to whoever should have it */
	if (server.seat->keyboard_state.focused_surface == xwayland_surface->surface)
		apply_keyboard_focus();
}

/* ---- the X server ---- */

/* Xwayland started: learn the atoms we need, give X11 a cursor. */
static void handle_ready(void *)
{
	xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(xwayland);
	std::array<xcb_intern_atom_cookie_t, std::size(floating_type_names)> cookies;
	for (size_t i = 0; i < floating_types.size(); i++)
		cookies[i] = xcb_intern_atom(connection, 0, strlen(floating_type_names[i]),
					     floating_type_names[i]);
	for (size_t i = 0; i < floating_types.size(); i++) {
		xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookies[i], nullptr);
		floating_types[i] = reply ? reply->atom : 0;
		free(reply);
	}
	wlr_xcursor_manager_load(server.xcursor_manager, 1);
	wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(server.xcursor_manager, "default", 1);
	if (xcursor) {
		wlr_xcursor_image *image = xcursor->images[0];
		wlr_xwayland_set_cursor(xwayland, image->buffer, image->width * 4, image->width,
					image->height, image->hotspot_x, image->hotspot_y);
	}
}

void xwayland_init()
{
	/* lazy: Xwayland starts when an X11 client first connects */
	xwayland = wlr_xwayland_create(server.display, server.compositor, true);
	if (!xwayland) {
		wlr_log(WLR_ERROR, "cannot start Xwayland: X11 applications will not run");
		unsetenv("DISPLAY");
		return;
	}
	xwayland_ready.listen(&xwayland->events.ready, handle_ready);
	new_xwayland_surface.listen(&xwayland->events.new_surface, [](void *data) {
		auto *surface = static_cast<wlr_xwayland_surface *>(data);
		if (surface->override_redirect)
			new X11Popup(surface);
		else
			new X11View(surface);
	});
	wlr_xwayland_set_seat(xwayland, server.seat);
	setenv("DISPLAY", xwayland->display_name, 1);
	wlr_log(WLR_INFO, "X11 applications use DISPLAY=%s", xwayland->display_name);
}

void xwayland_finish()
{
	if (!xwayland)
		return;
	xwayland_ready.stop();
	new_xwayland_surface.stop();
	wlr_xwayland_destroy(xwayland);
	xwayland = nullptr;
}
