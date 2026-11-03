/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* view.cpp - toplevels (Emacs frames and applications), Wayland windows
 * and popups, placement and keyboard focus. */
#include "waymacs.h"
#include <signal.h>

View *view_by_id(uint32_t id)
{
	for (View *view : server.views)
		if (view->id == id)
			return view;
	return nullptr;
}

bool frame_is_visible(View *frame)
{
	return frame && frame->is_frame && frame->mapped && frame->output &&
	       frame->output->active_frame == frame;
}

/* Emacs asked for its own frame: it covers a fullscreen application
 * (e.g. to show the minibuffer) instead of hiding behind it. */
static bool frame_is_revealed(View *frame)
{
	return frame && server.focused_view == frame && server.emacs_wants_frame;
}

Output *focused_output()
{
	View *frame = frame_of(server.focused_view);
	if (frame && frame->output)
		return frame->output;
	wlr_output *wlr_output = wlr_output_layout_output_at(server.output_layout,
							      server.cursor->x, server.cursor->y);
	if (wlr_output && wlr_output->data)
		return static_cast<Output *>(wlr_output->data);
	return server.outputs.empty() ? nullptr : server.outputs.front();
}

/* the Emacs frame shown on the focused output */
static View *current_frame()
{
	Output *output = focused_output();
	return output ? output->active_frame : nullptr;
}

static int frame_count(Output *output)
{
	return std::count_if(server.views.begin(), server.views.end(), [output](View *view) {
		return view->is_frame && view->output == output;
	});
}

/* Tell Emacs when OUTPUT shows no workspace: like i3, it gives every
 * output one. */
void ask_frame_for_empty_output(Output *output)
{
	if (output && !frame_count(output))
		ipc_send("(need-frame :output %s)", lisp_string(output->wlr_output->name));
}

/* OUTPUT's name as Lisp, nil for none */
static const char *lisp_output(Output *output)
{
	return output ? lisp_string(output->wlr_output->name) : "nil";
}

/* FRAME is now on OUTPUT (null: none).  Every change goes through here, so
 * that Emacs knows where its frames are. */
void frame_assign_output(View *frame, Output *output)
{
	if (frame->output == output)
		return;
	frame->output = output;
	ipc_send("(frame-placed :id %u :output %s)", frame->id, lisp_output(output));
}

/* If OUTPUT shows FRAME, it shows the frame focused before it instead. */
static void stop_showing(Output *output, View *frame)
{
	if (!output || output->active_frame != frame)
		return;
	View *best = nullptr;
	for (View *view : server.views)
		if (view != frame && view->is_frame && view->mapped &&
		    view->output == output && (!best || view->last_focused > best->last_focused))
			best = view;
	output->active_frame = best;
}

/* Show FRAME on its output, as the most recently focused one there. */
static void raise_frame(View *frame)
{
	if (frame->output)
		frame->output->active_frame = frame;
	frame->last_focused = ++server.focus_counter;
}

View::View(pid_t pid) : SceneOwner{VIEW}, id(server.next_view_id++), pid(pid)
{
	server.views.push_back(this);
}

View::~View()
{
	bar_view_unmapped(this);
	for (View *other : server.views)
		if (other->frame == this)
			other->frame = nullptr;
	for (Output *output : server.outputs)
		stop_showing(output, this);
	if (server.focused_view == this)
		server.focused_view = nullptr;
	end_grab(this);
	erase(server.views, this);
	ask_frame_for_empty_output(is_frame ? output : nullptr);
}

/* Clip the window (not its popups) to WIDTH x HEIGHT; WIDTH <= 0 removes
 * the clip. */
static void view_clip(View *view, int width, int height)
{
	wlr_scene_node *node = view->surface_node();
	if (!node)
		return;
	if (width <= 0) {
		wlr_scene_subsurface_tree_set_clip(node, nullptr);
		return;
	}
	wlr_box geometry = view->geometry();
	wlr_box clip = { geometry.x, geometry.y, width, height };
	wlr_scene_subsurface_tree_set_clip(node, &clip);
}

/* ---- Wayland (xdg-shell) windows ---- */

struct XdgView : View {
	XdgView(wlr_xdg_toplevel *xdg_toplevel, pid_t pid);
	void handle_commit();
	void set_decoration_mode();

	wlr_surface *surface() override { return toplevel->base->surface; }
	const char *title_or_null() override { return toplevel->title; }
	const char *app_id_or_null() override { return toplevel->app_id; }
	bool is_x11() override { return false; }

	wlr_box geometry() override
	{
#if WLR_VERSION_MINOR >= 19
		return toplevel->base->geometry;
#else
		wlr_box geometry = {};
		wlr_xdg_surface_get_geometry(toplevel->base, &geometry);
		return geometry;
#endif
	}

	void configure(int x, int y, int width, int height) override
	{
		if (!toplevel->base->initialized ||
		    (width == configured_width && height == configured_height))
			return;
		configured_width = width;
		configured_height = height;
		wlr_xdg_toplevel_set_size(toplevel, width, height);
	}

	void set_activated(bool activated) override
	{
		wlr_xdg_toplevel_set_activated(toplevel, activated);
	}

	void set_fullscreen_state(bool fullscreen) override
	{
		if (toplevel->base->initialized)
			wlr_xdg_toplevel_set_fullscreen(toplevel, fullscreen);
	}

	View *parent() override
	{
		return toplevel->parent ? static_cast<View *>(toplevel->parent->base->data) : nullptr;
	}

	bool has_fixed_size() override
	{
		const wlr_xdg_toplevel_state &state = toplevel->current;
		return state.min_width > 0 && state.min_height > 0 &&
		       state.min_width == state.max_width && state.min_height == state.max_height;
	}

	void close(bool kill_client) override
	{
		if (kill_client)
			wl_client_destroy(wl_resource_get_client(toplevel->resource));
		else
			wlr_xdg_toplevel_send_close(toplevel);
	}

	/* wlr_scene_xdg_surface_create() makes the surface tree the first
	 * child; popup trees are added after it. */
	wlr_scene_node *surface_node() override
	{
		wlr_scene_node *node;
		return wl_container_of(scene_tree->children.next, node, link);
	}

	void became_frame() override
	{
		if (!toplevel->base->initialized)
			return;
		wlr_xdg_toplevel_set_tiled(toplevel, 0);
		wlr_xdg_toplevel_set_maximized(toplevel, true);
	}

	wlr_xdg_toplevel *toplevel;
	wlr_xdg_toplevel_decoration_v1 *decoration = nullptr;
	Listener map, unmap, commit, destroy, set_title, set_app_id;
	Listener request_fullscreen, request_maximize, request_minimize;
	Listener request_move, request_resize;
	Listener decoration_request_mode, decoration_destroy;
};

XdgView::XdgView(wlr_xdg_toplevel *xdg_toplevel, pid_t pid) : View(pid), toplevel(xdg_toplevel)
{
	is_frame = server.emacs_pid && pid == server.emacs_pid;
	scene_tree = wlr_scene_xdg_surface_create(server.tile_tree, toplevel->base);
	scene_tree->node.data = static_cast<SceneOwner *>(this);
	wlr_scene_node_set_enabled(&scene_tree->node, false);
	toplevel->base->data = static_cast<View *>(this);
	toplevel->base->surface->data = scene_tree;	/* popups attach here */

	wlr_surface *surface = toplevel->base->surface;
	map.listen(&surface->events.map, [this](void *) { view_map(this); });
	unmap.listen(&surface->events.unmap, [this](void *) { view_unmap(this); });
	commit.listen(&surface->events.commit, [this](void *) { handle_commit(); });
	destroy.listen(&toplevel->events.destroy, [this](void *) { delete this; });
	set_title.listen(&toplevel->events.set_title, [this](void *) { view_title_changed(this); });
	set_app_id.listen(&toplevel->events.set_app_id, [this](void *) { view_send_update(this); });
	request_fullscreen.listen(&toplevel->events.request_fullscreen, [this](void *) {
		if (!toplevel->base->initialized)
			return;		/* handled by the initial commit */
		if (is_frame || !mapped)
			wlr_xdg_surface_schedule_configure(toplevel->base);
		else
			view_set_fullscreen(this, toplevel->requested.fullscreen);
	});
	/* maximize/minimize: tiling ignores them, but xdg-shell wants a reply */
	auto reply_with_configure = [this](void *) {
		if (toplevel->base->initialized)
			wlr_xdg_surface_schedule_configure(toplevel->base);
	};
	request_maximize.listen(&toplevel->events.request_maximize, reply_with_configure);
	request_minimize.listen(&toplevel->events.request_minimize, reply_with_configure);
	request_move.listen(&toplevel->events.request_move, [this](void *) {
		begin_grab(this, CursorMode::move, 0);
	});
	request_resize.listen(&toplevel->events.request_resize, [this](void *data) {
		begin_grab(this, CursorMode::resize,
			   static_cast<wlr_xdg_toplevel_resize_event *>(data)->edges);
	});
}

void XdgView::handle_commit()
{
	if (toplevel->base->initial_commit) {
		/* the first configure: decide the size up front */
		if (decoration)
			set_decoration_mode();
		wlr_xdg_toplevel_set_wm_capabilities(toplevel,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		if (is_frame) {
			/* Emacs names the frame's output in its title (see
			 * view_title_changed); without that, the focused one */
			if (!output)
				frame_assign_output(this, focused_output());
			wlr_xdg_toplevel_set_maximized(toplevel, true);
			wlr_xdg_toplevel_set_activated(toplevel, true);
			if (output) {
				configured_width = output->usable_box.width;
				configured_height = output->usable_box.height;
			}
		} else {
			wlr_xdg_toplevel_set_tiled(toplevel, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
						   WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
			if (toplevel->requested.fullscreen) {
				fullscreen = true;
				wlr_xdg_toplevel_set_fullscreen(toplevel, true);
			}
		}
		wlr_xdg_toplevel_set_size(toplevel, configured_width, configured_height);
		return;
	}
	/* the window geometry may have moved under the clip */
	if (mapped && !is_frame && in_layout && !floating && !fullscreen)
		view_clip(this, tile_box.width, tile_box.height);
}

/* We draw no decorations, and ask clients not to draw theirs either. */
void XdgView::set_decoration_mode()
{
	if (toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void handle_new_decoration(void *data)
{
	auto *decoration = static_cast<wlr_xdg_toplevel_decoration_v1 *>(data);
	auto *view = static_cast<XdgView *>(static_cast<View *>(decoration->toplevel->base->data));
	if (!view || view->decoration)
		return;
	view->decoration = decoration;
	view->decoration_request_mode.listen(&decoration->events.request_mode, [view](void *) {
		view->set_decoration_mode();
	});
	view->decoration_destroy.listen(&decoration->events.destroy, [view](void *) {
		view->decoration_request_mode.stop();
		view->decoration_destroy.stop();
		view->decoration = nullptr;
	});
	view->set_decoration_mode();
}

/* ---- the life of a window ---- */

/* Center a floating window on its parent window, or else on the output. */
static void place_floating(View *view)
{
	wlr_box geometry = view->geometry(), area = {};
	Output *output = view->frame && view->frame->output ? view->frame->output : focused_output();
	if (output)
		area = output->usable_box;
	int width = geometry.width > 0 ? geometry.width : 640;
	int height = geometry.height > 0 ? geometry.height : 480;
	if (area.width > 0)
		width = std::min(width, area.width * 9 / 10);
	if (area.height > 0)
		height = std::min(height, area.height * 9 / 10);
	View *parent = view->parent();
	if (parent && parent->mapped && parent->scene_tree->node.enabled) {
		wlr_box parent_geometry = parent->geometry();
		wlr_scene_node_coords(&parent->scene_tree->node, &area.x, &area.y);
		area.width = parent_geometry.width;
		area.height = parent_geometry.height;
	}
	view->floating_box = { area.x + (area.width - width) / 2,
			       area.y + (area.height - height) / 2, width, height };
}

/* Tell Emacs about an application window, including what it needs to
 * decide whether the window floats. */
static void view_announce(View *view)
{
	view->announced = true;
	const char *type = view->window_type();
	ipc_send("(new :id %u :app %s :title %s :pid %d :floating %s :parent %s :modal %s"
		 " :fixed-size %s :type %s)", view->id, lisp_string(view->app_id()),
		 lisp_string(view->title()), (int)view->pid, view->floating ? "t" : "nil",
		 view->parent() ? "t" : "nil", view->is_modal() ? "t" : "nil",
		 view->has_fixed_size() ? "t" : "nil", type ? type : "nil");
	if (view->fullscreen)
		ipc_send("(fullscreen :id %u :state t)", view->id);
}

static void frame_map(View *frame)
{
	if (!frame->output)
		frame_assign_output(frame, focused_output());
	wlr_scene_node_reparent(&frame->scene_tree->node, server.frame_tree);
	bool take_focus = !server.focused_view || focused_output() == frame->output;
	raise_frame(frame);
	if (take_focus)
		focus_view(frame, false);
	else
		arrange();
}

static void app_map(View *app)
{
	bar_view_mapped(app);
	app->frame = current_frame();
	/* Without Emacs the window floats.  Otherwise Emacs puts it in a
	 * window or floats it (waymacs-floating-function); only a fullscreen
	 * window is shown before that. */
	app->unmanaged = app->floating = !ipc_connected();
	if (app->unmanaged) {
		place_floating(app);
		focus_view(app, false);
		return;
	}
	view_announce(app);
	if (app->fullscreen) {
		focus_view(app, false);
		ipc_send("(activate :id %u)", app->id);
	} else {
		arrange();
	}
}

void view_map(View *view)
{
	view->mapped = true;
	if (view->is_frame)
		frame_map(view);
	else
		app_map(view);
}

static void focus_frame_on(Output *output)
{
	View *frame = output ? output->active_frame : nullptr;
	if (!frame) {
		auto found = std::find_if(server.views.begin(), server.views.end(), [](View *view) {
			return view->is_frame && view->mapped;
		});
		frame = found != server.views.end() ? *found : nullptr;
	}
	focus_view(frame, false);
}

void focus_fallback()
{
	focus_frame_on(focused_output());
}

void view_unmap(View *view)
{
	Output *output = server.focused_view == view ? focused_output() : nullptr;
	view->mapped = false;
	end_grab(view);
	if (view->is_frame) {
		stop_showing(view->output, view);
	} else {
		if (view->announced)
			ipc_send("(close :id %u)", view->id);
		bar_view_unmapped(view);
		view->announced = view->in_layout = view->floating = view->fullscreen = false;
		view->frame = nullptr;
	}
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	if (server.focused_view == view) {
		server.focused_view = nullptr;
		focus_frame_on(output);
	} else {
		arrange();
	}
}

void view_send_update(View *view)
{
	bar_view_updated(view);
	bar_update();
	if (view->announced)
		ipc_send("(update :id %u :app %s :title %s)", view->id,
			 lisp_string(view->app_id()), lisp_string(view->title()));
}

void view_title_changed(View *view)
{
	const char *title = view->title();
	/* Emacs pairs its frames with our ids by titling them
	 * "waymacs-frame-N" until we answer with a `frame' event; with
	 * "@OUTPUT" after it, the frame goes on that output. */
	if (strncmp(title, "waymacs-frame-", 14)) {
		view_send_update(view);
		return;
	}
	const char *at = strchr(title, '@');
	view->tag.assign(title, at ? at - title : strlen(title));
	if (!view->is_frame)
		return;
	if (at)
		frame_set_output(view, output_by_name(at + 1, true));
	ipc_send("(frame :id %u :tag %s :output %s)", view->id, lisp_string(view->tag.c_str()),
		 lisp_output(view->output));
}

/* ---- popups (menus, tooltips) ---- */

struct Popup {
	Popup(wlr_xdg_popup *popup, wlr_scene_tree *parent_tree);
	void handle_commit();
	void handle_unmap();
	wlr_xdg_popup *xdg_popup;
	Listener commit, map, unmap, destroy;
};

void Popup::handle_commit()
{
	if (!xdg_popup->base->initial_commit)
		return;
	/* keep the popup on the output its root window is on */
	wlr_surface *root = xdg_popup->parent;
	wlr_xdg_surface *xdg_surface;
	while (root && (xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root)) &&
	       xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP)
		root = xdg_surface->popup->parent;
	auto *root_tree = root ? static_cast<wlr_scene_tree *>(root->data) : nullptr;
	wlr_output *wlr_output = nullptr;
	int root_x = 0, root_y = 0;
	if (root_tree) {
		wlr_scene_node_coords(&root_tree->node, &root_x, &root_y);
		wlr_output = wlr_output_layout_output_at(server.output_layout, root_x, root_y);
	}
	if (!wlr_output && focused_output())
		wlr_output = focused_output()->wlr_output;
	if (wlr_output) {
		wlr_box box;
		wlr_output_layout_get_box(server.output_layout, wlr_output, &box);
		box.x -= root_x;
		box.y -= root_y;
		wlr_xdg_popup_unconstrain_from_box(xdg_popup, &box);
	}
	wlr_xdg_surface_schedule_configure(xdg_popup->base);
}

/* Focus SURFACE even during a popup grab (wlr_seat_keyboard_notify_enter
 * would go through the grab, which ignores it). */
void keyboard_enter_now(wlr_surface *surface)
{
	wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (keyboard)
		wlr_seat_keyboard_enter(server.seat, surface, keyboard->keycodes,
					keyboard->num_keycodes, &keyboard->modifiers);
	else
		wlr_seat_keyboard_enter(server.seat, surface, nullptr, 0, nullptr);
}

void Popup::handle_unmap()
{
	if (server.locked || server.seat->keyboard_state.focused_surface != xdg_popup->base->surface)
		return;
	/* back to the parent menu, or to whatever has the keyboard otherwise */
	wlr_xdg_popup *parent = xdg_popup->parent ?
		wlr_xdg_popup_try_from_wlr_surface(xdg_popup->parent) : nullptr;
	wlr_surface *target = parent && parent->seat ? parent->base->surface : nullptr;
	if (!target && server.focused_layer && server.focused_layer->mapped)
		target = server.focused_layer->layer_surface->surface;
	if (!target && server.focused_view && server.focused_view->mapped)
		target = server.focused_view->surface();
	if (target)
		keyboard_enter_now(target);
	else
		wlr_seat_keyboard_clear_focus(server.seat);
}

Popup::Popup(wlr_xdg_popup *popup, wlr_scene_tree *parent_tree) : xdg_popup(popup)
{
	wlr_scene_tree *tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
	wlr_surface *surface = xdg_popup->base->surface;
	surface->data = tree;	/* nested popups attach here */
	commit.listen(&surface->events.commit, [this](void *) { handle_commit(); });
	/* xdg-shell: the topmost popup holding a grab (an open menu) has the
	 * keyboard.  wlroots starts the grab but leaves the focus to us. */
	map.listen(&surface->events.map, [this](void *) {
		if (xdg_popup->seat && !server.locked)
			keyboard_enter_now(xdg_popup->base->surface);
	});
	unmap.listen(&surface->events.unmap, [this](void *) { handle_unmap(); });
	destroy.listen(&xdg_popup->events.destroy, [this](void *) { delete this; });
}

void popup_create(wlr_xdg_popup *xdg_popup, wlr_scene_tree *parent_tree)
{
	new Popup(xdg_popup, parent_tree);
}

/* ---- placement ---- */

static void show(View *view, wlr_scene_tree *parent_tree, int x, int y)
{
	wlr_scene_node *node = &view->scene_tree->node;
	if (node->parent != parent_tree)
		wlr_scene_node_reparent(node, parent_tree);
	wlr_scene_node_set_position(node, x, y);
	wlr_scene_node_set_enabled(node, true);
}

static void arrange_frame(View *frame)
{
	Output *output = frame->output;
	wlr_scene_node_set_enabled(&frame->scene_tree->node, frame_is_visible(frame));
	if (!output)
		return;
	const wlr_box &box = output->usable_box;
	wlr_scene_node_set_position(&frame->scene_tree->node, box.x, box.y);
	frame->configure(box.x, box.y, box.width, box.height);
}

static void arrange_app(View *app)
{
	View *frame = app->frame;
	Output *output = frame && frame->output ? frame->output : focused_output();
	bool visible = app->unmanaged || frame_is_visible(frame);
	/* While Emacs reads input (a minibuffer prompt), a fullscreen window
	 * leaves Emacs's minibuffer visible below it; without knowing where
	 * that is, it steps aside entirely. */
	bool prompting = output && app->fullscreen && frame_is_revealed(frame);
	int visible_height = output ? output->box.height : 0;
	if (prompting)
		visible_height = frame->minibuffer_top > 0 ?
			frame->scene_tree->node.y + frame->minibuffer_top - output->box.y : 0;
	if (!visible || visible_height <= 0) {
		wlr_scene_node_set_enabled(&app->scene_tree->node, false);
	} else if (app->fullscreen) {
		const wlr_box &box = output->box;
		/* clipped, not resized: the application does not redraw */
		view_clip(app, prompting ? box.width : 0, visible_height);
		app->configure(box.x, box.y, box.width, box.height);
		show(app, server.fullscreen_tree, box.x, box.y);
	} else if (app->floating) {
		const wlr_box &box = app->floating_box;
		view_clip(app, 0, 0);
		app->configure(box.x, box.y, box.width, box.height);
		show(app, server.floating_tree, box.x, box.y);
	} else if (app->in_layout) {
		/* the tile is relative to the frame's window geometry origin */
		const wlr_box &box = app->tile_box;
		int x = frame->scene_tree->node.x + box.x, y = frame->scene_tree->node.y + box.y;
		app->configure(x, y, box.width, box.height);
		view_clip(app, box.width, box.height);
		show(app, server.tile_tree, x, y);
	} else {
		wlr_scene_node_set_enabled(&app->scene_tree->node, false);
	}
}

void arrange()
{
	for (Output *output : server.outputs) {
		wlr_scene_node_set_position(&output->background->node, output->box.x, output->box.y);
		wlr_scene_rect_set_size(output->background, output->box.width, output->box.height);
	}
	/* frames first: applications are placed relative to them */
	for (View *view : server.views)
		if (!view->mapped)
			wlr_scene_node_set_enabled(&view->scene_tree->node, false);
		else if (view->is_frame)
			arrange_frame(view);
	for (View *view : server.views)
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
	wlr_surface *target = nullptr;
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
		target = server.focused_view->surface();
	if (!target) {
		wlr_seat_keyboard_notify_clear_focus(server.seat);
		return;
	}
	if (server.seat->keyboard_state.focused_surface == target)
		return;
	wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
	if (!keyboard) {
		wlr_seat_keyboard_notify_enter(server.seat, target, nullptr, 0, nullptr);
		return;
	}
	std::vector<uint32_t> pressed;
	std::copy_if(keyboard->keycodes, keyboard->keycodes + keyboard->num_keycodes,
		     std::back_inserter(pressed),
		     [excluded_keycode](uint32_t keycode) { return keycode != excluded_keycode; });
	wlr_seat_keyboard_notify_enter(server.seat, target, pressed.data(), pressed.size(),
				       &keyboard->modifiers);
}

static void set_focus(View *view, bool requested_by_emacs)
{
	View *previous = server.focused_view;
	server.focused_view = view;
	server.emacs_wants_frame = requested_by_emacs && view && view->is_frame;
	/* focusing a view shows the frame (workspace) it is on */
	if (view && view->is_frame)
		raise_frame(view);
	else if (view && view->frame && view->frame->mapped && view->frame->output)
		raise_frame(view->frame);
	if (view && !view->is_frame && view->floating)
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
	if (previous != view) {
		if (previous && !previous->is_frame && previous->mapped)
			previous->set_activated(false);
		if (view && !view->is_frame)
			view->set_activated(true);
	}
	arrange();
}

void focus_view(View *view, bool requested_by_emacs)
{
	if (view && !view->mapped)
		return;
	set_focus(view, requested_by_emacs);
	apply_keyboard_focus();
}

/* Focus an Emacs frame because KEYCODE was intercepted; the caller then
 * replays that key as a fresh press. */
void focus_frame_for_key(View *frame, uint32_t keycode)
{
	set_focus(frame, false);
	apply_keyboard_focus(keycode);
}

/* ---- requests from Emacs ---- */

void layout_begin()
{
	for (View *view : server.views)
		if (!view->is_frame)
			view->in_layout = false;
}

void layout_add(uint32_t frame_id, uint32_t app_id, int x, int y, int width, int height)
{
	View *frame = view_by_id(frame_id), *app = view_by_id(app_id);
	if (!frame || !frame->is_frame || !app || app->is_frame || !app->mapped ||
	    width <= 0 || height <= 0)
		return;
	/* one window in several Emacs windows: the first visible one wins */
	if (app->in_layout && frame_is_visible(app->frame))
		return;
	app->in_layout = true;
	app->frame = frame;
	app->floating = app->unmanaged = false;
	app->tile_box = { x, y, width, height };
}

/* After a layout: the focused application may now be on another frame
 * (moved to another workspace); show that workspace. */
void layout_end()
{
	View *app = server.focused_view;
	if (app && !app->is_frame && app->frame && app->frame->mapped &&
	    app->frame->output && !frame_is_visible(app->frame))
		raise_frame(app->frame);
	arrange();
}

/* Emacs (re)connected: learn its pid and tell it everything. */
void emacs_hello(pid_t pid)
{
	server.emacs_pid = pid;
	for (View *view : server.views)
		view->announced = false;
	for (View *view : server.views) {
		bool belongs_to_emacs = view->pid == pid && !view->is_x11();
		if (belongs_to_emacs == view->is_frame)
			continue;
		/* the guess made at startup was wrong: reclassify */
		bool was_mapped = view->mapped;
		view->mapped = false;
		view->is_frame = belongs_to_emacs;
		view->in_layout = view->floating = view->fullscreen = view->unmanaged = false;
		view->frame = nullptr;
		view->output = nullptr;
		view->configured_width = view->configured_height = -1;
		if (server.focused_view == view)
			server.focused_view = nullptr;
		for (Output *output : server.outputs)
			stop_showing(output, view);
		if (belongs_to_emacs) {
			bar_view_unmapped(view);	/* off the taskbar */
			view->became_frame();
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
	for (Output *output : server.disabled_outputs)
		ipc_send("(output-off :name %s)", lisp_string(output->wlr_output->name));
	if (server.lid_closed)
		ipc_send("(lid :closed t)");
	for (View *view : server.views) {
		if (view->is_frame && !view->tag.empty())
			ipc_send("(frame :id %u :tag %s :output %s)", view->id,
				 lisp_string(view->tag.c_str()), lisp_output(view->output));
		if (view->is_frame || !view->mapped || view->announced)
			continue;
		/* a window mapped without Emacs floats until Emacs decides */
		view->unmanaged = false;
		view_announce(view);
	}
	for (Output *output : server.outputs)
		ask_frame_for_empty_output(output);
	bar_hello();
	arrange();
}

void view_set_fullscreen(View *view, bool fullscreen)
{
	if (view->is_frame)
		return;
	view->fullscreen = fullscreen;
	view->set_fullscreen_state(fullscreen);
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

void view_set_floating(View *view, bool floating)
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

/* Put a floating window on FRAME's workspace (null: hidden until the
 * workspace exists). */
void view_set_frame(View *app, View *frame)
{
	if (frame && !frame->is_frame)
		frame = nullptr;
	Output *old_output = app->frame ? app->frame->output : nullptr;
	app->frame = frame;
	if (app->floating && frame && frame->output != old_output)
		place_floating(app);	/* it moved to another monitor */
	if (server.focused_view == app && !frame_is_visible(frame))
		focus_fallback();
	else
		arrange();
}

static void place_floating_apps_of(View *frame)
{
	for (View *app : server.views)
		if (app->frame == frame && app->floating)
			place_floating(app);
}

/* Move a workspace (an Emacs frame) to OUTPUT and show it there. */
void frame_move_to_output(View *frame, Output *output)
{
	Output *old_output = frame->output;
	if (old_output == output)
		return;
	stop_showing(old_output, frame);
	frame_assign_output(frame, output);
	place_floating_apps_of(frame);
	focus_view(frame, true);
	ask_frame_for_empty_output(old_output);
}

/* Put FRAME on OUTPUT without focusing it, unlike frame_move_to_output():
 * the new output shows it if it shows nothing else, or if FRAME (or an
 * application on it) has the focus. */
void frame_set_output(View *frame, Output *output)
{
	Output *old_output = frame->output;
	if (!output || old_output == output)
		return;
	frame_assign_output(frame, output);
	if (!frame->mapped)
		return;		/* frame_map() shows it */
	stop_showing(old_output, frame);
	if (!frame_is_visible(output->active_frame) || frame_of(server.focused_view) == frame)
		output->active_frame = frame;
	place_floating_apps_of(frame);
	arrange();
	ask_frame_for_empty_output(old_output);
}

/* Something (xdg-activation, a taskbar) wants VIEW shown; Emacs decides
 * for the windows it manages. */
void view_request_activate(View *view)
{
	if (!view->mapped)
		return;
	if (view->is_frame || view->unmanaged || !ipc_connected())
		focus_view(view, false);
	else
		ipc_send("(request-activate :id %u)", view->id);
}

void view_init()
{
	wlr_xdg_shell *xdg_shell = wlr_xdg_shell_create(server.display, 6);
	server.listen(&xdg_shell->events.new_toplevel, [](void *data) {
		auto *toplevel = static_cast<wlr_xdg_toplevel *>(data);
		pid_t pid;
		wl_client_get_credentials(wl_resource_get_client(toplevel->resource), &pid,
					  nullptr, nullptr);
		new XdgView(toplevel, pid);
	});
	server.listen(&xdg_shell->events.new_popup, [](void *data) {
		auto *xdg_popup = static_cast<wlr_xdg_popup *>(data);
		/* layer-shell popups get their parent later, see output.cpp */
		if (xdg_popup->parent && xdg_popup->parent->data)
			popup_create(xdg_popup, static_cast<wlr_scene_tree *>(xdg_popup->parent->data));
	});
	server.listen(&wlr_xdg_decoration_manager_v1_create(server.display)
		      ->events.new_toplevel_decoration, handle_new_decoration);
	/* An application asks for attention (xdg-activation); Emacs decides. */
	server.listen(&wlr_xdg_activation_v1_create(server.display)->events.request_activate,
		      [](void *data) {
		auto *event = static_cast<wlr_xdg_activation_v1_request_activate_event *>(data);
		wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(event->surface);
		if (toplevel && toplevel->base->data)
			view_request_activate(static_cast<View *>(toplevel->base->data));
	});
}
