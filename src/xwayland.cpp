/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* xwayland.cpp - X11 applications, through Xwayland.
 *
 * Ordinary X11 windows become views like Wayland ones: Emacs gets a
 * buffer for each.  Override-redirect windows (menus, tooltips, drop-down
 * lists) place themselves; we only show them, above everything but the
 * lock screen.  Xwayland is started when the first X11 client connects. */
#include "waymacs.h"

/* override-redirect windows */
struct x11_popup {
	struct wlr_xwayland_surface *xwayland_surface;
	struct wlr_scene_tree *surface_tree;	/* while mapped */
	struct wl_listener associate, dissociate, map, unmap, request_configure;
	struct wl_listener set_geometry, set_override_redirect, destroy;
};

/* window types (_NET_WM_WINDOW_TYPE) that float like dialogs */
static const char *const floating_type_names[] = {
	"_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
	"_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_SPLASH",
};
#define FLOATING_TYPE_COUNT (sizeof floating_type_names / sizeof floating_type_names[0])
static xcb_atom_t floating_types[FLOATING_TYPE_COUNT];

static void view_create_x11(struct wlr_xwayland_surface *xwayland_surface);
static void popup_create_x11(struct wlr_xwayland_surface *xwayland_surface);

/* dialogs, splash screens, and windows that cannot be resized */
bool xwayland_wants_to_float(struct wlr_xwayland_surface *surface)
{
	if (surface->modal || surface->parent)
		return true;
	for (size_t i = 0; i < surface->window_type_len; i++)
		for (size_t j = 0; j < FLOATING_TYPE_COUNT; j++)
			if (floating_types[j] && surface->window_type[i] == floating_types[j])
				return true;
	xcb_size_hints_t *hints = surface->size_hints;
	return hints && hints->min_width > 0 && hints->min_height > 0 &&
	       hints->min_width == hints->max_width && hints->min_height == hints->max_height;
}

/* ---- ordinary windows: views ---- */

static void handle_view_map(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, map);
	struct wlr_xwayland_surface *xwayland_surface = view->xwayland_surface;
	view->surface_tree = wlr_scene_subsurface_tree_create(view->scene_tree,
							      xwayland_surface->surface);
	view->pid = xwayland_surface->pid;
	view->fullscreen = xwayland_surface->fullscreen;
	view->configured_x = view->configured_y = INT32_MIN;	/* configure it again */
	view_map(view);
}

static void handle_view_unmap(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, unmap);
	wlr_scene_node_destroy(&view->surface_tree->node);
	view->surface_tree = NULL;
	view_unmap(view);
}

/* the X11 window got its Wayland surface */
static void handle_view_associate(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, associate);
	struct wlr_surface *surface = view->xwayland_surface->surface;
	LISTEN(&surface->events.map, &view->map, handle_view_map);
	LISTEN(&surface->events.unmap, &view->unmap, handle_view_unmap);
}

/* X11 clients size and move their windows themselves.  Before mapping
 * they get what they ask for; afterwards, floating windows may resize,
 * the others are told their place again. */
static void handle_view_request_configure(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_configure);
	auto *event = (struct wlr_xwayland_surface_configure_event *)data;
	struct wlr_xwayland_surface *xwayland_surface = view->xwayland_surface;
	if (!view->mapped) {
		wlr_xwayland_surface_configure(xwayland_surface, event->x, event->y,
					       event->width, event->height);
		return;
	}
	if (view->floating && !view->fullscreen) {
		view->floating_box.width = event->width;
		view->floating_box.height = event->height;
	}
	view->configured_x = view->configured_y = INT32_MIN;
	arrange();
	if (view->configured_x == INT32_MIN)	/* not shown: answer anyway */
		wlr_xwayland_surface_configure(xwayland_surface, xwayland_surface->x,
					       xwayland_surface->y, xwayland_surface->width,
					       xwayland_surface->height);
}

static void handle_view_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_fullscreen);
	if (view->mapped)
		view_set_fullscreen(view, view->xwayland_surface->fullscreen);
}

static void handle_view_request_resize(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_resize);
	auto *event = (struct wlr_xwayland_resize_event *)data;
	if (view->floating && !view->fullscreen)
		begin_grab(view, CURSOR_RESIZE, event->edges);
}

static void view_destroy_x11(struct view *view)
{
	view->xwayland_surface->data = NULL;
	wlr_scene_node_destroy(&view->scene_tree->node);
	view_destroy(view);
}

/* An unmapped window may turn into an override-redirect one, or back. */
static void handle_view_set_override_redirect(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_override_redirect);
	struct wlr_xwayland_surface *xwayland_surface = view->xwayland_surface;
	if (view->mapped)
		handle_view_unmap(&view->unmap, NULL);
	view_destroy_x11(view);
	popup_create_x11(xwayland_surface);
}

static void view_create_x11(struct wlr_xwayland_surface *xwayland_surface)
{
	struct view *view = view_create(xwayland_surface->pid);
	view->xwayland_surface = xwayland_surface;
	view->scene_tree = wlr_scene_tree_create(server.tile_tree);
	view->scene_tree->node.data = view;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	xwayland_surface->data = view;

	struct wlr_xwayland_surface *surface = xwayland_surface;
	LISTEN(&surface->events.associate, &view->associate, handle_view_associate);
	LISTEN(&surface->events.dissociate, &view->dissociate,
	       [](struct wl_listener *listener, void *data) {
		struct view *view = OWNER(listener, struct view, dissociate);
		stop_listening(&view->map, &view->unmap);
	});
	LISTEN(&surface->events.destroy, &view->destroy, [](struct wl_listener *listener, void *data) {
		view_destroy_x11(OWNER(listener, struct view, destroy));
	});
	LISTEN(&surface->events.request_configure, &view->request_configure,
	       handle_view_request_configure);
	LISTEN(&surface->events.request_activate, &view->request_activate,
	       [](struct wl_listener *listener, void *data) {
		view_request_activate(OWNER(listener, struct view, request_activate));
	});
	LISTEN(&surface->events.request_fullscreen, &view->request_fullscreen,
	       handle_view_request_fullscreen);
	LISTEN(&surface->events.request_move, &view->request_move, view_handle_request_move);
	LISTEN(&surface->events.request_resize, &view->request_resize,
	       handle_view_request_resize);
	LISTEN(&surface->events.set_title, &view->set_title, view_handle_set_title);
	LISTEN(&surface->events.set_class, &view->set_app_id, view_handle_set_app_id);
	LISTEN(&surface->events.set_override_redirect, &view->set_override_redirect,
	       handle_view_set_override_redirect);
	if (surface->surface) {
		handle_view_associate(&view->associate, NULL);
		if (surface->surface->mapped)
			handle_view_map(&view->map, NULL);
	}
}

/* ---- override-redirect windows ---- */

static void handle_popup_map(struct wl_listener *listener, void *data)
{
	struct x11_popup *popup = wl_container_of(listener, popup, map);
	struct wlr_xwayland_surface *xwayland_surface = popup->xwayland_surface;
	popup->surface_tree = wlr_scene_subsurface_tree_create(server.x11_menu_tree,
							       xwayland_surface->surface);
	wlr_scene_node_set_position(&popup->surface_tree->node, xwayland_surface->x,
				    xwayland_surface->y);
#if WLR_VERSION_MINOR >= 19
	bool wants_focus = wlr_xwayland_surface_override_redirect_wants_focus(xwayland_surface);
#else
	bool wants_focus = wlr_xwayland_or_surface_wants_focus(xwayland_surface);
#endif
	if (!server.locked && wants_focus)
		keyboard_enter_now(xwayland_surface->surface);
}

static void handle_popup_unmap(struct wl_listener *listener, void *data)
{
	struct x11_popup *popup = wl_container_of(listener, popup, unmap);
	wlr_scene_node_destroy(&popup->surface_tree->node);
	popup->surface_tree = NULL;
	/* the keyboard goes back to whoever should have it */
	if (server.seat->keyboard_state.focused_surface == popup->xwayland_surface->surface)
		apply_keyboard_focus(UINT32_MAX);
}

static void handle_popup_associate(struct wl_listener *listener, void *data)
{
	struct x11_popup *popup = wl_container_of(listener, popup, associate);
	struct wlr_surface *surface = popup->xwayland_surface->surface;
	LISTEN(&surface->events.map, &popup->map, handle_popup_map);
	LISTEN(&surface->events.unmap, &popup->unmap, handle_popup_unmap);
}

static void handle_popup_request_configure(struct wl_listener *listener, void *data)
{
	struct x11_popup *popup = wl_container_of(listener, popup, request_configure);
	auto *event = (struct wlr_xwayland_surface_configure_event *)data;
	wlr_xwayland_surface_configure(popup->xwayland_surface, event->x, event->y,
				       event->width, event->height);
}

static void handle_popup_set_geometry(struct wl_listener *listener, void *data)
{
	struct x11_popup *popup = wl_container_of(listener, popup, set_geometry);
	if (popup->surface_tree)
		wlr_scene_node_set_position(&popup->surface_tree->node,
					    popup->xwayland_surface->x,
					    popup->xwayland_surface->y);
}

static void popup_destroy_x11(struct x11_popup *popup)
{
	stop_listening(&popup->associate, &popup->dissociate, &popup->map, &popup->unmap,
		       &popup->request_configure, &popup->set_geometry,
		       &popup->set_override_redirect, &popup->destroy);
	free(popup);
}

static void handle_popup_set_override_redirect(struct wl_listener *listener, void *data)
{
	struct x11_popup *popup = wl_container_of(listener, popup, set_override_redirect);
	struct wlr_xwayland_surface *xwayland_surface = popup->xwayland_surface;
	if (popup->surface_tree)
		handle_popup_unmap(&popup->unmap, NULL);
	popup_destroy_x11(popup);
	view_create_x11(xwayland_surface);
}

static void popup_create_x11(struct wlr_xwayland_surface *xwayland_surface)
{
	struct x11_popup *popup = (struct x11_popup *)calloc(1, sizeof *popup);
	popup->xwayland_surface = xwayland_surface;
	struct wlr_xwayland_surface *surface = xwayland_surface;
	LISTEN(&surface->events.associate, &popup->associate, handle_popup_associate);
	LISTEN(&surface->events.dissociate, &popup->dissociate,
	       [](struct wl_listener *listener, void *data) {
		struct x11_popup *popup = OWNER(listener, struct x11_popup, dissociate);
		stop_listening(&popup->map, &popup->unmap);
	});
	LISTEN(&surface->events.request_configure, &popup->request_configure,
	       handle_popup_request_configure);
	LISTEN(&surface->events.set_geometry, &popup->set_geometry, handle_popup_set_geometry);
	LISTEN(&surface->events.set_override_redirect, &popup->set_override_redirect,
	       handle_popup_set_override_redirect);
	LISTEN(&surface->events.destroy, &popup->destroy, [](struct wl_listener *listener, void *data) {
		popup_destroy_x11(OWNER(listener, struct x11_popup, destroy));
	});
	if (surface->surface) {
		handle_popup_associate(&popup->associate, NULL);
		if (surface->surface->mapped)
			handle_popup_map(&popup->map, NULL);
	}
}

/* ---- the X server ---- */

static void handle_new_surface(struct wl_listener *listener, void *data)
{
	auto *xwayland_surface = (struct wlr_xwayland_surface *)data;
	if (xwayland_surface->override_redirect)
		popup_create_x11(xwayland_surface);
	else
		view_create_x11(xwayland_surface);
}

/* Xwayland started: learn the atoms we need, give X11 a cursor. */
static void handle_ready(struct wl_listener *listener, void *data)
{
	xcb_connection_t *connection = wlr_xwayland_get_xwm_connection(server.xwayland);
	xcb_intern_atom_cookie_t cookies[FLOATING_TYPE_COUNT];
	for (size_t i = 0; i < FLOATING_TYPE_COUNT; i++)
		cookies[i] = xcb_intern_atom(connection, 0, strlen(floating_type_names[i]),
					     floating_type_names[i]);
	for (size_t i = 0; i < FLOATING_TYPE_COUNT; i++) {
		xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookies[i],
								       NULL);
		floating_types[i] = reply ? reply->atom : 0;
		free(reply);
	}
	wlr_xcursor_manager_load(server.xcursor_manager, 1);
	struct wlr_xcursor *xcursor =
		wlr_xcursor_manager_get_xcursor(server.xcursor_manager, "default", 1);
	if (xcursor) {
		struct wlr_xcursor_image *image = xcursor->images[0];
		wlr_xwayland_set_cursor(server.xwayland, image->buffer, image->width * 4,
					image->width, image->height, image->hotspot_x,
					image->hotspot_y);
	}
}

void xwayland_init(void)
{
	/* lazy: Xwayland starts when an X11 client first connects */
	server.xwayland = wlr_xwayland_create(server.display, server.compositor, true);
	if (!server.xwayland) {
		wlr_log(WLR_ERROR, "cannot start Xwayland: X11 applications will not run");
		unsetenv("DISPLAY");
		return;
	}
	LISTEN(&server.xwayland->events.ready, &server.xwayland_ready, handle_ready);
	LISTEN(&server.xwayland->events.new_surface, &server.new_xwayland_surface,
	       handle_new_surface);
	wlr_xwayland_set_seat(server.xwayland, server.seat);
	setenv("DISPLAY", server.xwayland->display_name, 1);
	wlr_log(WLR_INFO, "X11 applications use DISPLAY=%s", server.xwayland->display_name);
}

void xwayland_finish(void)
{
	if (!server.xwayland)
		return;
	stop_listening(&server.xwayland_ready, &server.new_xwayland_surface);
	wlr_xwayland_destroy(server.xwayland);
	server.xwayland = NULL;
}
