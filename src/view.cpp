/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* view.cpp - toplevels (Emacs frames and applications), popups,
 * placement and keyboard focus. */
#include "waymacs.h"
#include <signal.h>

struct popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit, map, unmap, destroy;
};

struct view *view_by_id(uint32_t id)
{
	struct view *view;
	wl_list_for_each(view, &server.views, link)
		if (view->id == id)
			return view;
	return NULL;
}

bool frame_is_visible(struct view *frame)
{
	return frame && frame->is_frame && frame->mapped && frame->output &&
	       frame->output->active_frame == frame;
}

/* Emacs asked for its own frame: it covers a fullscreen application
 * (e.g. to show the minibuffer) instead of hiding behind it. */
static bool frame_is_revealed(struct view *frame)
{
	return frame && server.focused_view == frame && server.emacs_wants_frame;
}

struct output *focused_output(void)
{
	struct view *view = server.focused_view;
	if (view && view->is_frame && view->output)
		return view->output;
	if (view && !view->is_frame && view->frame && view->frame->output)
		return view->frame->output;
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
		server.output_layout, server.cursor->x, server.cursor->y);
	if (wlr_output && wlr_output->data)
		return (struct output *)wlr_output->data;
	if (wl_list_empty(&server.outputs))
		return NULL;
	struct output *first_output = wl_container_of(server.outputs.next, first_output, link);
	return first_output;
}

/* the Emacs frame shown on the focused output */
static struct view *current_frame(void)
{
	struct output *output = focused_output();
	return output ? output->active_frame : NULL;
}

static int frame_count(struct output *output)
{
	int count = 0;
	struct view *view;
	wl_list_for_each(view, &server.views, link)
		if (view->is_frame && view->output == output)
			count++;
	return count;
}

static void ask_frame_for_empty_output(struct output *output);

/* New frames go to outputs without one first; then they stack (like
 * workspaces) on the output that has the focus. */
static struct output *output_for_new_frame(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link)
		if (!frame_count(output))
			return output;
	return focused_output();
}

/* the most recently focused mapped frame of OUTPUT, except EXCLUDED */
static struct view *last_focused_frame(struct output *output, struct view *excluded)
{
	struct view *best = NULL, *view;
	wl_list_for_each(view, &server.views, link)
		if (view != excluded && view->is_frame && view->mapped &&
		    view->output == output && (!best || view->last_focused > best->last_focused))
			best = view;
	return best;
}

/* ---- the two kinds of windows: Wayland (xdg-shell) and X11 ---- */

struct wlr_surface *view_surface(struct view *view)
{
	if (view->xwayland_surface)
		return view->xwayland_surface->surface;
	return view->toplevel->base->surface;
}

const char *view_title(struct view *view)
{
	const char *title = view->xwayland_surface ? view->xwayland_surface->title :
		view->toplevel->title;
	return title ? title : "";
}

/* the Wayland app id, or the X11 class */
const char *view_app_id(struct view *view)
{
	const char *app_id = view->xwayland_surface ? view->xwayland_surface->class_ :
		view->toplevel->app_id;
	return app_id ? app_id : "";
}

static struct wlr_box view_geometry(struct view *view)
{
	struct wlr_box geometry = { 0, 0, 0, 0 };
	if (view->xwayland_surface) {
		geometry.width = view->xwayland_surface->width;
		geometry.height = view->xwayland_surface->height;
	} else {
#if WLR_VERSION_MINOR >= 19
		geometry = view->toplevel->base->geometry;
#else
		wlr_xdg_surface_get_geometry(view->toplevel->base, &geometry);
#endif
	}
	return geometry;
}

/* Give the window its size.  X11 windows also learn their position (X,
 * Y in layout coordinates): their menus are placed with it. */
static void view_configure(struct view *view, int x, int y, int width, int height)
{
	if (view->xwayland_surface) {
		if (x == view->configured_x && y == view->configured_y &&
		    width == view->configured_width && height == view->configured_height)
			return;
		view->configured_x = x;
		view->configured_y = y;
		view->configured_width = width;
		view->configured_height = height;
		wlr_xwayland_surface_configure(view->xwayland_surface, x, y, width, height);
		return;
	}
	if (!view->toplevel->base->initialized ||
	    (width == view->configured_width && height == view->configured_height))
		return;
	view->configured_width = width;
	view->configured_height = height;
	wlr_xdg_toplevel_set_size(view->toplevel, width, height);
}

static void view_set_activated(struct view *view, bool activated)
{
	if (view->xwayland_surface) {
		wlr_xwayland_surface_activate(view->xwayland_surface, activated);
		if (activated)
			wlr_xwayland_surface_restack(view->xwayland_surface, NULL,
						     XCB_STACK_MODE_ABOVE);
	} else {
		wlr_xdg_toplevel_set_activated(view->toplevel, activated);
	}
}

/* the window it is a dialog of */
static struct view *view_parent(struct view *view)
{
	if (view->xwayland_surface)
		return view->xwayland_surface->parent ?
			(struct view *)view->xwayland_surface->parent->data : NULL;
	return view->toplevel->parent ? (struct view *)view->toplevel->parent->base->data : NULL;
}

/* Clip the window (not its popups) to WIDTH x HEIGHT; WIDTH <= 0 removes
 * the clip. */
static void view_clip(struct view *view, int width, int height)
{
	/* wlr_scene_xdg_surface_create() makes the surface tree the first
	 * child; popup trees are added after it. */
	struct wlr_scene_node *surface_node;
	if (view->xwayland_surface) {
		if (!view->surface_tree)
			return;
		surface_node = &view->surface_tree->node;
	} else {
		surface_node = wl_container_of(view->scene_tree->children.next, surface_node, link);
	}
	if (width <= 0) {
		wlr_scene_subsurface_tree_set_clip(surface_node, NULL);
		return;
	}
	struct wlr_box geometry = view_geometry(view);
	struct wlr_box clip = { geometry.x, geometry.y, width, height };
	wlr_scene_subsurface_tree_set_clip(surface_node, &clip);
}

/* dialogs, and windows that cannot be resized */
static bool wants_to_float(struct view *view)
{
	if (view->xwayland_surface)
		return xwayland_wants_to_float(view->xwayland_surface);
	struct wlr_xdg_toplevel_state *state = &view->toplevel->current;
	if (view->toplevel->parent)
		return true;
	return state->min_width > 0 && state->min_height > 0 &&
	       state->min_width == state->max_width && state->min_height == state->max_height;
}

/* Center a floating window on its parent window, or else on the output. */
static void place_floating(struct view *view)
{
	struct wlr_box geometry = view_geometry(view), area = { 0, 0, 0, 0 };
	struct output *output = view->frame && view->frame->output ?
		view->frame->output : focused_output();
	if (output)
		area = output->usable_box;
	int width = geometry.width > 0 ? geometry.width : 640;
	int height = geometry.height > 0 ? geometry.height : 480;
	if (area.width > 0 && width > area.width * 9 / 10)
		width = area.width * 9 / 10;
	if (area.height > 0 && height > area.height * 9 / 10)
		height = area.height * 9 / 10;
	struct view *parent = view_parent(view);
	if (parent && parent->mapped && parent->scene_tree->node.enabled) {
		struct wlr_box parent_geometry = view_geometry(parent);
		wlr_scene_node_coords(&parent->scene_tree->node, &area.x, &area.y);
		area.width = parent_geometry.width;
		area.height = parent_geometry.height;
	}
	view->floating_box.x = area.x + (area.width - width) / 2;
	view->floating_box.y = area.y + (area.height - height) / 2;
	view->floating_box.width = width;
	view->floating_box.height = height;
}

static void view_announce(struct view *view)
{
	view->announced = true;
	ipc_send("(new :id %u :app %s :title %s :pid %d :floating %s)", view->id,
		 lisp_string(view_app_id(view)), lisp_string(view_title(view)),
		 (int)view->pid, view->floating ? "t" : "nil");
	if (view->fullscreen)
		ipc_send("(fullscreen :id %u :state t)", view->id);
}

static void frame_map(struct view *frame)
{
	if (!frame->output)
		frame->output = output_for_new_frame();
	wlr_scene_node_reparent(&frame->scene_tree->node, server.frame_tree);
	bool take_focus = !server.focused_view || focused_output() == frame->output;
	if (frame->output)
		frame->output->active_frame = frame;
	frame->last_focused = ++server.focus_counter;
	if (take_focus)
		focus_view(frame, false);
	else
		arrange();
}

static void app_map(struct view *app)
{
	bar_view_mapped(app);
	app->frame = current_frame();
	app->unmanaged = !ipc_connected();
	app->floating = app->unmanaged || wants_to_float(app);
	if (app->floating)
		place_floating(app);
	if (!app->unmanaged)
		view_announce(app);
	if (app->floating || app->fullscreen) {
		focus_view(app, false);
		if (!app->unmanaged)
			ipc_send("(activate :id %u)", app->id);
	} else {
		arrange();	/* hidden until Emacs puts it in a window */
	}
}

void view_map(struct view *view)
{
	view->mapped = true;
	if (view->is_frame)
		frame_map(view);
	else
		app_map(view);
}

static void focus_frame_on(struct output *output)
{
	struct view *frame = output ? output->active_frame : NULL, *view;
	if (!frame) {
		wl_list_for_each(view, &server.views, link) {
			if (view->is_frame && view->mapped) {
				frame = view;
				break;
			}
		}
	}
	focus_view(frame, false);
}

void focus_fallback(void)
{
	focus_frame_on(focused_output());
}

void view_unmap(struct view *view)
{
	struct output *output = server.focused_view == view ? focused_output() : NULL;
	view->mapped = false;
	if (server.grabbed_view == view) {
		server.grabbed_view = NULL;
		server.cursor_mode = CURSOR_PASSTHROUGH;
	}
	if (view->is_frame) {
		if (view->output && view->output->active_frame == view)
			view->output->active_frame = last_focused_frame(view->output, view);
	} else {
		if (view->announced)
			ipc_send("(close :id %u)", view->id);
		bar_view_unmapped(view);
		view->announced = view->in_layout = view->floating = view->fullscreen = false;
		view->frame = NULL;
	}
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	if (server.focused_view == view) {
		server.focused_view = NULL;
		focus_frame_on(output);
	} else {
		arrange();
	}
}

static void handle_commit(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, commit);
	struct wlr_xdg_toplevel *toplevel = view->toplevel;
	if (toplevel->base->initial_commit) {
		/* the first configure: decide the size up front */
		if (view->decoration)
			wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		wlr_xdg_toplevel_set_wm_capabilities(toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		if (view->is_frame) {
			if (!view->output)
				view->output = output_for_new_frame();
			wlr_xdg_toplevel_set_maximized(toplevel, true);
			wlr_xdg_toplevel_set_activated(toplevel, true);
			if (view->output) {
				view->configured_width = view->output->usable_box.width;
				view->configured_height = view->output->usable_box.height;
			}
		} else {
			wlr_xdg_toplevel_set_tiled(toplevel, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
						   WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
			if (toplevel->requested.fullscreen) {
				view->fullscreen = true;
				wlr_xdg_toplevel_set_fullscreen(toplevel, true);
			}
		}
		wlr_xdg_toplevel_set_size(toplevel, view->configured_width,
					  view->configured_height);
		return;
	}
	/* the window geometry may have moved under the clip */
	if (view->mapped && !view->is_frame && view->in_layout && !view->floating &&
	    !view->fullscreen)
		view_clip(view, view->tile_box.width, view->tile_box.height);
}

void view_send_update(struct view *view)
{
	bar_view_updated(view);
	bar_update();
	if (view->announced)
		ipc_send("(update :id %u :app %s :title %s)", view->id,
			 lisp_string(view_app_id(view)), lisp_string(view_title(view)));
}

/* for Wayland and X11 windows alike */
void view_handle_set_title(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, set_title);
	const char *title = view_title(view);
	/* Emacs pairs its frames with our ids by titling them
	 * "waymacs-frame-N" until we answer with a `frame' event; with
	 * "@OUTPUT" after it, the frame goes on that output. */
	if (!strncmp(title, "waymacs-frame-", 14)) {
		const char *at = strchr(title, '@');
		int tag_length = at ? (int)(at - title) : (int)strlen(title);
		snprintf(view->tag, sizeof view->tag, "%.*s", tag_length, title);
		if (view->is_frame) {
			if (at && output_by_name(at + 1, true))
				frame_set_output(view, output_by_name(at + 1, true));
			ipc_send("(frame :id %u :tag %s)", view->id, lisp_string(view->tag));
		}
		return;
	}
	view_send_update(view);
}

void view_handle_set_app_id(struct wl_listener *listener, void *data)
{
	view_send_update(OWNER(listener, struct view, set_app_id));
}

void view_handle_request_move(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_move);
	if (view->floating && !view->fullscreen)
		begin_grab(view, CURSOR_MOVE, 0);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_fullscreen);
	if (!view->toplevel->base->initialized)
		return;		/* handled by the initial commit */
	if (view->is_frame || !view->mapped)
		wlr_xdg_surface_schedule_configure(view->toplevel->base);
	else
		view_set_fullscreen(view, view->toplevel->requested.fullscreen);
}

static void handle_request_resize(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, request_resize);
	auto *event = (struct wlr_xdg_toplevel_resize_event *)data;
	if (view->floating && !view->fullscreen)
		begin_grab(view, CURSOR_RESIZE, event->edges);
}

/* maximize/minimize: tiling ignores them, but xdg-shell wants a reply */
static void reply_with_configure(struct view *view)
{
	if (view->toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(view->toplevel->base);
}

struct view *view_create(pid_t pid)
{
	struct view *view = (struct view *)calloc(1, sizeof *view);
	view->type = OWNER_VIEW;
	view->id = server.next_view_id++;
	view->pid = pid;
	view->configured_x = view->configured_y = INT32_MIN;
	wl_list_insert(server.views.prev, &view->link);
	return view;
}

void view_destroy(struct view *view)
{
	/* the listeners of both kinds of windows: those not added are skipped */
	stop_listening(&view->map, &view->unmap, &view->commit, &view->destroy,
		       &view->set_title, &view->set_app_id, &view->request_fullscreen,
		       &view->request_maximize, &view->request_minimize, &view->request_move,
		       &view->request_resize, &view->associate, &view->dissociate,
		       &view->request_configure, &view->request_activate,
		       &view->set_override_redirect, &view->decoration_request_mode,
		       &view->decoration_destroy);
	bar_view_unmapped(view);
	struct view *other;
	wl_list_for_each(other, &server.views, link)
		if (other->frame == view)
			other->frame = NULL;
	struct output *output;
	wl_list_for_each(output, &server.outputs, link)
		if (output->active_frame == view)
			output->active_frame = last_focused_frame(output, view);
	if (server.focused_view == view)
		server.focused_view = NULL;
	if (server.grabbed_view == view) {
		server.grabbed_view = NULL;
		server.cursor_mode = CURSOR_PASSTHROUGH;
	}
	struct output *frame_output = view->is_frame ? view->output : NULL;
	wl_list_remove(&view->link);
	free(view);
	ask_frame_for_empty_output(frame_output);
}

static void handle_new_toplevel(struct wl_listener *listener, void *data)
{
	auto *toplevel = (struct wlr_xdg_toplevel *)data;
	pid_t pid;
	wl_client_get_credentials(wl_resource_get_client(toplevel->resource), &pid, NULL, NULL);
	struct view *view = view_create(pid);
	view->toplevel = toplevel;
	view->is_frame = server.emacs_pid && view->pid == server.emacs_pid;
	view->scene_tree = wlr_scene_xdg_surface_create(server.tile_tree, toplevel->base);
	view->scene_tree->node.data = view;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	toplevel->base->data = view;
	toplevel->base->surface->data = view->scene_tree;	/* popups attach here */

	struct wlr_surface *surface = toplevel->base->surface;
	LISTEN(&surface->events.map, &view->map, [](struct wl_listener *listener, void *data) {
		view_map(OWNER(listener, struct view, map));
	});
	LISTEN(&surface->events.unmap, &view->unmap, [](struct wl_listener *listener, void *data) {
		view_unmap(OWNER(listener, struct view, unmap));
	});
	LISTEN(&surface->events.commit, &view->commit, handle_commit);
	LISTEN(&toplevel->events.destroy, &view->destroy,
	       [](struct wl_listener *listener, void *data) {
		view_destroy(OWNER(listener, struct view, destroy));
	});
	LISTEN(&toplevel->events.set_title, &view->set_title, view_handle_set_title);
	LISTEN(&toplevel->events.set_app_id, &view->set_app_id, view_handle_set_app_id);
	LISTEN(&toplevel->events.request_fullscreen, &view->request_fullscreen,
	       handle_request_fullscreen);
	LISTEN(&toplevel->events.request_maximize, &view->request_maximize,
	       [](struct wl_listener *listener, void *data) {
		reply_with_configure(OWNER(listener, struct view, request_maximize));
	});
	LISTEN(&toplevel->events.request_minimize, &view->request_minimize,
	       [](struct wl_listener *listener, void *data) {
		reply_with_configure(OWNER(listener, struct view, request_minimize));
	});
	LISTEN(&toplevel->events.request_move, &view->request_move, view_handle_request_move);
	LISTEN(&toplevel->events.request_resize, &view->request_resize, handle_request_resize);
}

/* ---- popups (menus, tooltips) ---- */

static void handle_popup_commit(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, commit);
	if (!popup->xdg_popup->base->initial_commit)
		return;
	/* keep the popup on the output its root window is on */
	struct wlr_surface *root = popup->xdg_popup->parent;
	struct wlr_xdg_surface *xdg_surface;
	while (root && (xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root)) &&
	       xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP)
		root = xdg_surface->popup->parent;
	struct wlr_scene_tree *root_tree = root ? (struct wlr_scene_tree *)root->data : NULL;
	struct wlr_output *wlr_output = NULL;
	int root_x = 0, root_y = 0;
	if (root_tree) {
		wlr_scene_node_coords(&root_tree->node, &root_x, &root_y);
		wlr_output = wlr_output_layout_output_at(server.output_layout, root_x, root_y);
	}
	if (!wlr_output && focused_output())
		wlr_output = focused_output()->wlr_output;
	if (wlr_output) {
		struct wlr_box box;
		wlr_output_layout_get_box(server.output_layout, wlr_output, &box);
		box.x -= root_x;
		box.y -= root_y;
		wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &box);
	}
	wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

/* Focus SURFACE even during a popup grab (wlr_seat_keyboard_notify_enter
 * would go through the grab, which ignores it). */
void keyboard_enter_now(struct wlr_surface *surface)
{
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard)
		wlr_seat_keyboard_enter(server.seat, surface, keyboard->keycodes,
					keyboard->num_keycodes, &keyboard->modifiers);
	else
		wlr_seat_keyboard_enter(server.seat, surface, NULL, 0, NULL);
}

/* xdg-shell: the topmost popup holding a grab (an open menu) has the
 * keyboard.  wlroots starts the grab but leaves the focus to us. */
static void handle_popup_map(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, map);
	if (popup->xdg_popup->seat && !server.locked)
		keyboard_enter_now(popup->xdg_popup->base->surface);
}

static void handle_popup_unmap(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, unmap);
	if (server.locked ||
	    server.seat->keyboard_state.focused_surface != popup->xdg_popup->base->surface)
		return;
	/* back to the parent menu, or to whatever has the keyboard otherwise */
	struct wlr_xdg_popup *parent = popup->xdg_popup->parent ?
		wlr_xdg_popup_try_from_wlr_surface(popup->xdg_popup->parent) : NULL;
	struct wlr_surface *target = parent && parent->seat ? parent->base->surface : NULL;
	if (!target && server.focused_layer && server.focused_layer->mapped)
		target = server.focused_layer->layer_surface->surface;
	if (!target && server.focused_view && server.focused_view->mapped)
		target = view_surface(server.focused_view);
	if (target)
		keyboard_enter_now(target);
	else
		wlr_seat_keyboard_clear_focus(server.seat);
}

static void handle_popup_destroy(struct wl_listener *listener, void *data)
{
	struct popup *popup = wl_container_of(listener, popup, destroy);
	stop_listening(&popup->commit, &popup->map, &popup->unmap, &popup->destroy);
	free(popup);
}

void popup_create(struct wlr_xdg_popup *xdg_popup, struct wlr_scene_tree *parent_tree)
{
	struct popup *popup = (struct popup *)calloc(1, sizeof *popup);
	popup->xdg_popup = xdg_popup;
	struct wlr_scene_tree *tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
	struct wlr_surface *surface = xdg_popup->base->surface;
	surface->data = tree;	/* nested popups attach here */
	LISTEN(&surface->events.commit, &popup->commit, handle_popup_commit);
	LISTEN(&surface->events.map, &popup->map, handle_popup_map);
	LISTEN(&surface->events.unmap, &popup->unmap, handle_popup_unmap);
	LISTEN(&xdg_popup->events.destroy, &popup->destroy, handle_popup_destroy);
}

static void handle_new_popup(struct wl_listener *listener, void *data)
{
	auto *xdg_popup = (struct wlr_xdg_popup *)data;
	/* layer-shell popups get their parent later, see output.cpp */
	if (xdg_popup->parent && xdg_popup->parent->data)
		popup_create(xdg_popup, (struct wlr_scene_tree *)xdg_popup->parent->data);
}

/* ---- placement ---- */

static void show(struct view *view, struct wlr_scene_tree *parent_tree, int x, int y)
{
	struct wlr_scene_node *node = &view->scene_tree->node;
	if (node->parent != parent_tree)
		wlr_scene_node_reparent(node, parent_tree);
	wlr_scene_node_set_position(node, x, y);
	wlr_scene_node_set_enabled(node, true);
}

static void arrange_frame(struct view *frame)
{
	struct output *output = frame->output;
	wlr_scene_node_set_enabled(&frame->scene_tree->node, frame_is_visible(frame));
	if (!output)
		return;
	wlr_scene_node_set_position(&frame->scene_tree->node,
				    output->usable_box.x, output->usable_box.y);
	view_configure(frame, output->usable_box.x, output->usable_box.y,
		       output->usable_box.width, output->usable_box.height);
}

static void arrange_app(struct view *app)
{
	struct view *frame = app->frame;
	struct output *output = frame && frame->output ? frame->output : focused_output();
	bool visible = app->unmanaged || frame_is_visible(frame);
	/* While Emacs reads input (a minibuffer prompt), a fullscreen window
	 * leaves Emacs's minibuffer visible below it; without knowing where
	 * that is, it steps aside entirely. */
	bool prompting = app->fullscreen && frame_is_revealed(frame);
	int visible_height = output ? output->box.height : 0;
	if (prompting)
		visible_height = frame->minibuffer_top > 0 ?
			frame->scene_tree->node.y + frame->minibuffer_top - output->box.y : 0;
	if (!visible || !output || visible_height <= 0) {
		wlr_scene_node_set_enabled(&app->scene_tree->node, false);
	} else if (app->fullscreen) {
		struct wlr_box *box = &output->box;
		/* clipped, not resized: the application does not redraw */
		view_clip(app, prompting ? box->width : 0, visible_height);
		view_configure(app, box->x, box->y, box->width, box->height);
		show(app, server.fullscreen_tree, box->x, box->y);
	} else if (app->floating) {
		struct wlr_box *box = &app->floating_box;
		view_clip(app, 0, 0);
		view_configure(app, box->x, box->y, box->width, box->height);
		show(app, server.floating_tree, box->x, box->y);
	} else if (app->in_layout) {
		/* the tile is relative to the frame's window geometry origin */
		struct wlr_box *box = &app->tile_box;
		struct wlr_scene_node *frame_node = &frame->scene_tree->node;
		int x = frame_node->x + box->x, y = frame_node->y + box->y;
		view_configure(app, x, y, box->width, box->height);
		view_clip(app, box->width, box->height);
		show(app, server.tile_tree, x, y);
	} else {
		wlr_scene_node_set_enabled(&app->scene_tree->node, false);
	}
}

void arrange(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		wlr_scene_node_set_position(&output->background->node,
					    output->box.x, output->box.y);
		wlr_scene_rect_set_size(output->background, output->box.width,
					output->box.height);
	}
	/* frames first: applications are placed relative to them */
	struct view *view;
	wl_list_for_each(view, &server.views, link)
		if (!view->mapped)
			wlr_scene_node_set_enabled(&view->scene_tree->node, false);
		else if (view->is_frame)
			arrange_frame(view);
	wl_list_for_each(view, &server.views, link)
		if (view->mapped && !view->is_frame)
			arrange_app(view);
	lock_arrange();
	bar_update();
	update_pointer_focus();
}

/* ---- keyboard focus ---- */

/* Give the keyboard to the layer surface or view that should have it.
 * EXCLUDED_KEYCODE is left out of the keys reported as pressed in the
 * enter event, so that key can be replayed as a real press. */
void apply_keyboard_focus(uint32_t excluded_keycode)
{
	struct wlr_surface *target = NULL;
	if (server.locked) {
		/* only the locker gets keys; wlr_seat_keyboard_enter ignores grabs */
		target = lock_keyboard_target();
		if (!target)
			wlr_seat_keyboard_clear_focus(server.seat);
		else if (server.seat->keyboard_state.focused_surface != target)
			keyboard_enter_now(target);
		return;
	}
	if (server.focused_layer && server.focused_layer->mapped)
		target = server.focused_layer->layer_surface->surface;
	else if (server.focused_view && server.focused_view->mapped)
		target = view_surface(server.focused_view);
	if (!target) {
		wlr_seat_keyboard_notify_clear_focus(server.seat);
		return;
	}
	if (server.seat->keyboard_state.focused_surface == target)
		return;
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (!keyboard) {
		wlr_seat_keyboard_notify_enter(server.seat, target, NULL, 0, NULL);
		return;
	}
	uint32_t pressed[WLR_KEYBOARD_KEYS_CAP];
	size_t pressed_count = 0;
	for (size_t i = 0; i < keyboard->num_keycodes; i++)
		if (keyboard->keycodes[i] != excluded_keycode)
			pressed[pressed_count++] = keyboard->keycodes[i];
	wlr_seat_keyboard_notify_enter(server.seat, target, pressed, pressed_count,
				       &keyboard->modifiers);
}

static void set_focus(struct view *view, bool requested_by_emacs)
{
	struct view *previous = server.focused_view;
	server.focused_view = view;
	server.emacs_wants_frame = requested_by_emacs && view && view->is_frame;
	if (view && view->is_frame) {
		if (view->output)
			view->output->active_frame = view;
		view->last_focused = ++server.focus_counter;
	}
	/* focusing an application shows the frame (workspace) it is on */
	if (view && !view->is_frame && view->frame && view->frame->mapped &&
	    view->frame->output) {
		view->frame->output->active_frame = view->frame;
		view->frame->last_focused = ++server.focus_counter;
	}
	if (view && !view->is_frame && view->floating)
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
	if (previous != view) {
		if (previous && !previous->is_frame && previous->mapped)
			view_set_activated(previous, false);
		if (view && !view->is_frame)
			view_set_activated(view, true);
	}
	arrange();
}

void focus_view(struct view *view, bool requested_by_emacs)
{
	if (view && !view->mapped)
		return;
	set_focus(view, requested_by_emacs);
	apply_keyboard_focus(UINT32_MAX);
}

/* Focus an Emacs frame because KEYCODE was intercepted; the caller then
 * replays that key as a fresh press. */
void focus_frame_for_key(struct view *frame, uint32_t keycode)
{
	set_focus(frame, false);
	apply_keyboard_focus(keycode);
}

/* ---- requests from Emacs ---- */

void layout_begin(void)
{
	struct view *view;
	wl_list_for_each(view, &server.views, link)
		if (!view->is_frame)
			view->in_layout = false;
}

void layout_add(uint32_t frame_id, uint32_t app_id, int x, int y, int width, int height)
{
	struct view *frame = view_by_id(frame_id), *app = view_by_id(app_id);
	if (!frame || !frame->is_frame || !app || app->is_frame || !app->mapped ||
	    width <= 0 || height <= 0)
		return;
	/* one window in several Emacs windows: the first visible one wins */
	if (app->in_layout && frame_is_visible(app->frame))
		return;
	app->in_layout = true;
	app->frame = frame;
	app->floating = app->unmanaged = false;
	app->tile_box.x = x;
	app->tile_box.y = y;
	app->tile_box.width = width;
	app->tile_box.height = height;
}

/* After a layout: the focused application may now be on another frame
 * (moved to another workspace); show that workspace. */
void layout_end(void)
{
	struct view *app = server.focused_view;
	if (app && !app->is_frame && app->frame && app->frame->mapped &&
	    app->frame->output && !frame_is_visible(app->frame)) {
		app->frame->output->active_frame = app->frame;
		app->frame->last_focused = ++server.focus_counter;
	}
	arrange();
}

/* Emacs (re)connected: learn its pid and tell it everything. */
void emacs_hello(pid_t pid)
{
	server.emacs_pid = pid;
	struct view *view;
	wl_list_for_each(view, &server.views, link)
		view->announced = false;
	wl_list_for_each(view, &server.views, link) {
		bool belongs_to_emacs = view->pid == pid && !view->xwayland_surface;
		if (belongs_to_emacs == view->is_frame)
			continue;
		/* the guess made at startup was wrong: reclassify */
		bool was_mapped = view->mapped;
		view->mapped = false;
		view->is_frame = belongs_to_emacs;
		view->in_layout = view->floating = view->fullscreen = view->unmanaged = false;
		view->frame = NULL;
		view->output = NULL;
		view->configured_width = view->configured_height = -1;
		if (server.focused_view == view)
			server.focused_view = NULL;
		struct output *output;
		wl_list_for_each(output, &server.outputs, link)
			if (output->active_frame == view)
				output->active_frame = last_focused_frame(output, view);
		if (belongs_to_emacs && view->toplevel->base->initialized) {
			wlr_xdg_toplevel_set_tiled(view->toplevel, 0);
			wlr_xdg_toplevel_set_maximized(view->toplevel, true);
		}
		if (was_mapped) {
			view->mapped = true;
			if (belongs_to_emacs)
				frame_map(view);
			else
				app_map(view);
		}
	}
	outputs_announce();
	struct output *disabled_output;
	wl_list_for_each(disabled_output, &server.disabled_outputs, link)
		ipc_send("(output-off :name %s)", lisp_string(disabled_output->wlr_output->name));
	if (server.lid_closed)
		ipc_send("(lid :closed t)");
	wl_list_for_each(view, &server.views, link) {
		if (view->is_frame && view->tag[0])
			ipc_send("(frame :id %u :tag %s)", view->id, lisp_string(view->tag));
		if (view->is_frame || !view->mapped || view->announced)
			continue;
		if (view->unmanaged) {
			view->unmanaged = false;
			view->floating = wants_to_float(view);
			if (view->floating)
				place_floating(view);
		}
		view_announce(view);
	}
	struct output *output;
	wl_list_for_each(output, &server.outputs, link)
		if (!frame_count(output))
			ipc_send("(need-frame :output %s)",
				 lisp_string(output->wlr_output->name));
	bar_hello();
	arrange();
}

void view_set_fullscreen(struct view *view, bool fullscreen)
{
	if (view->is_frame)
		return;
	view->fullscreen = fullscreen;
	if (view->xwayland_surface)
		wlr_xwayland_surface_set_fullscreen(view->xwayland_surface, fullscreen);
	else if (view->toplevel->base->initialized)
		wlr_xdg_toplevel_set_fullscreen(view->toplevel, fullscreen);
	if (view->announced)
		ipc_send("(fullscreen :id %u :state %s)", view->id, fullscreen ? "t" : "nil");
	if (!view->mapped)
		return;
	if (!view->frame)
		view->frame = current_frame();
	if (fullscreen)
		focus_view(view, false);
	else
		arrange();
}

void view_set_floating(struct view *view, bool floating)
{
	if (view->is_frame || !view->mapped || view->floating == floating)
		return;
	view->floating = floating;
	view->in_layout = false;
	if (!floating) {
		arrange();
		return;
	}
	if (!frame_is_visible(view->frame))
		view->frame = current_frame();
	place_floating(view);
	focus_view(view, false);
}

void view_close(struct view *view, bool kill_client)
{
	if (view->is_frame)
		return;
	if (view->xwayland_surface) {
		/* all X11 clients share Xwayland's Wayland connection */
		if (kill_client && view->xwayland_surface->pid > 0)
			kill(view->xwayland_surface->pid, SIGKILL);
		else
			wlr_xwayland_surface_close(view->xwayland_surface);
	} else if (kill_client) {
		wl_client_destroy(wl_resource_get_client(view->toplevel->resource));
	} else {
		wlr_xdg_toplevel_send_close(view->toplevel);
	}
}

/* Like i3, every output always shows a workspace. */
static void ask_frame_for_empty_output(struct output *output)
{
	if (output && !frame_count(output))
		ipc_send("(need-frame :output %s)", lisp_string(output->wlr_output->name));
}

/* Put a floating window on FRAME's workspace (NULL: hidden until the
 * workspace exists). */
void view_set_frame(struct view *app, struct view *frame)
{
	if (frame && !frame->is_frame)
		frame = NULL;
	struct output *old_output = app->frame ? app->frame->output : NULL;
	app->frame = frame;
	if (app->floating && frame && frame->output != old_output)
		place_floating(app);	/* it moved to another monitor */
	if (server.focused_view == app && !frame_is_visible(frame))
		focus_fallback();
	else
		arrange();
}

/* Move a workspace (an Emacs frame) to OUTPUT and show it there. */
void frame_move_to_output(struct view *frame, struct output *output)
{
	struct output *old_output = frame->output;
	if (old_output == output)
		return;
	if (old_output && old_output->active_frame == frame)
		old_output->active_frame = last_focused_frame(old_output, frame);
	frame->output = output;
	struct view *app;
	wl_list_for_each(app, &server.views, link)
		if (app->frame == frame && app->floating)
			place_floating(app);
	focus_view(frame, true);
	ask_frame_for_empty_output(old_output);
}

/* does FRAME, or the application holding the focus, have the focus? */
static bool frame_has_focus(struct view *frame)
{
	struct view *view = server.focused_view;
	return view && (view == frame || (!view->is_frame && view->frame == frame));
}

/* Put FRAME on OUTPUT without focusing it, unlike frame_move_to_output():
 * the new output shows it if it shows nothing else. */
void frame_set_output(struct view *frame, struct output *output)
{
	struct output *old_output = frame->output;
	if (!output || old_output == output)
		return;
	frame->output = output;
	if (!frame->mapped)
		return;		/* frame_map() shows it */
	if (old_output && old_output->active_frame == frame)
		old_output->active_frame = last_focused_frame(old_output, frame);
	if (!frame_is_visible(output->active_frame) || frame_has_focus(frame))
		output->active_frame = frame;
	struct view *app;
	wl_list_for_each(app, &server.views, link)
		if (app->frame == frame && app->floating)
			place_floating(app);
	arrange();
	ask_frame_for_empty_output(old_output);
}

/* ---- decorations, activation ---- */

static void handle_decoration_request_mode(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, decoration_request_mode);
	if (view->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void handle_decoration_destroy(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, decoration_destroy);
	stop_listening(&view->decoration_request_mode, &view->decoration_destroy);
	view->decoration = NULL;
}

/* We draw no decorations, and ask clients not to draw theirs either. */
static void handle_new_decoration(struct wl_listener *listener, void *data)
{
	auto *decoration = (struct wlr_xdg_toplevel_decoration_v1 *)data;
	struct view *view = (struct view *)decoration->toplevel->base->data;
	if (!view || view->decoration)
		return;
	view->decoration = decoration;
	LISTEN(&decoration->events.request_mode, &view->decoration_request_mode,
	       handle_decoration_request_mode);
	LISTEN(&decoration->events.destroy, &view->decoration_destroy,
	       handle_decoration_destroy);
	handle_decoration_request_mode(&view->decoration_request_mode, decoration);
}

/* An application asks for attention (xdg-activation); Emacs decides. */
static void handle_request_activate(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_xdg_activation_v1_request_activate_event *)data;
	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(event->surface);
	struct view *view = toplevel ? (struct view *)toplevel->base->data : NULL;
	if (view)
		view_request_activate(view);
}

/* Something (xdg-activation, a taskbar) wants VIEW shown; Emacs decides
 * for the windows it manages. */
void view_request_activate(struct view *view)
{
	if (!view->mapped)
		return;
	if (view->is_frame || view->unmanaged || !ipc_connected())
		focus_view(view, false);
	else
		ipc_send("(request-activate :id %u)", view->id);
}

void view_init(void)
{
	server.xdg_shell = wlr_xdg_shell_create(server.display, 6);
	LISTEN(&server.xdg_shell->events.new_toplevel, &server.new_toplevel,
	       handle_new_toplevel);
	LISTEN(&server.xdg_shell->events.new_popup, &server.new_popup, handle_new_popup);
	struct wlr_xdg_decoration_manager_v1 *decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server.display);
	LISTEN(&decoration_manager->events.new_toplevel_decoration, &server.new_decoration,
	       handle_new_decoration);
	struct wlr_xdg_activation_v1 *activation = wlr_xdg_activation_v1_create(server.display);
	LISTEN(&activation->events.request_activate, &server.request_activate,
	       handle_request_activate);
}
