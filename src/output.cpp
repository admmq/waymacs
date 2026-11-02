/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* output.cpp - outputs and layer-shell surfaces (bars, wallpapers,
 * launchers, notifications). */
#include "waymacs.h"

void outputs_announce()
{
	for (Output *output : server.outputs)
		ipc_send("(output :name %s :x %d :y %d :width %d :height %d :scale %.3f)",
			 lisp_string(output->wlr_output->name), output->box.x, output->box.y,
			 output->box.width, output->box.height, output->wlr_output->scale);
}

/* ---- layer shell ---- */

/* Configure OUTPUT's layer surfaces that have (EXCLUSIVE) or have not an
 * exclusive zone; the zones are taken out of USABLE_BOX. */
static void configure_layers(Output *output, bool exclusive, wlr_box &usable_box)
{
	for (auto list = output->layers.rbegin(); list != output->layers.rend(); ++list)
		for (Layer *layer : *list)
			if (layer->layer_surface->initialized &&
			    (layer->layer_surface->current.exclusive_zone > 0) == exclusive)
				wlr_scene_layer_surface_v1_configure(layer->scene_layer, &output->box,
								     &usable_box);
}

/* the topmost top/overlay surface of OUTPUT that wants the keyboard for itself */
static Layer *exclusive_keyboard_layer(Output *output)
{
	for (int index : { ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, ZWLR_LAYER_SHELL_V1_LAYER_TOP }) {
		const std::vector<Layer *> &list = output->layers[index];
		auto found = std::find_if(list.rbegin(), list.rend(), [](Layer *layer) {
			return layer->mapped &&
			       layer->keyboard_interactivity_is(ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
		});
		if (found != list.rend())
			return *found;
	}
	return nullptr;
}

static void layers_arrange(Output *output)
{
	wlr_box usable_box = output->box;
	configure_layers(output, true, usable_box);
	configure_layers(output, false, usable_box);
	bool usable_box_changed = memcmp(&usable_box, &output->usable_box, sizeof usable_box) != 0;
	output->usable_box = usable_box;

	Layer *keyboard_layer = exclusive_keyboard_layer(output);
	if (keyboard_layer && server.focused_layer != keyboard_layer) {
		server.focused_layer = keyboard_layer;
		apply_keyboard_focus();
	}
	if (usable_box_changed)
		arrange();	/* Emacs frames are resized to the usable box */
}

static void layer_commit(Layer *layer)
{
	wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
	if (!layer->output)
		return;
	if (layer_surface->initial_commit) {
		/* arrange with the pending state so the first configure fits */
		wlr_layer_surface_v1_state old_state = layer_surface->current;
		layer_surface->current = layer_surface->pending;
		layers_arrange(layer->output);
		layer_surface->current = old_state;
		return;
	}
	if (layer_surface->current.committed == 0 && layer->mapped == layer_surface->surface->mapped)
		return;
	bool just_mapped = !layer->mapped && layer_surface->surface->mapped;
	layer->mapped = layer_surface->surface->mapped;
	if (layer_surface->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
		zwlr_layer_shell_v1_layer index = layer_surface->current.layer;
		wlr_scene_node_reparent(&layer->scene_layer->tree->node, server.layer_trees[index]);
		for (std::vector<Layer *> &list : layer->output->layers)
			erase(list, layer);
		layer->output->layers[index].push_back(layer);
	}
	layers_arrange(layer->output);
	/* launchers and the like want the keyboard as soon as they appear */
	if (just_mapped && layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
	    !layer->keyboard_interactivity_is(ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)) {
		server.focused_layer = layer;
		apply_keyboard_focus();
	}
}

static void unfocus_layer(Layer *layer)
{
	if (server.focused_layer != layer)
		return;
	server.focused_layer = nullptr;
	apply_keyboard_focus();
}

Layer::Layer(wlr_layer_surface_v1 *surface, Output *output)
	: SceneOwner{LAYER}, layer_surface(surface), output(output)
{
	zwlr_layer_shell_v1_layer index = surface->pending.layer;
	scene_layer = wlr_scene_layer_surface_v1_create(server.layer_trees[index], surface);
	scene_layer->tree->node.data = static_cast<SceneOwner *>(this);
	surface->surface->data = scene_layer->tree;	/* popups attach here */
	surface->data = this;
	commit.listen(&surface->surface->events.commit, [this](void *) { layer_commit(this); });
	unmap.listen(&surface->surface->events.unmap, [this](void *) {
		mapped = false;
		unfocus_layer(this);
		if (this->output)
			layers_arrange(this->output);
	});
	destroy.listen(&surface->events.destroy, [this](void *) { delete this; });
	new_popup.listen(&surface->events.new_popup, [this](void *data) {
		popup_create(static_cast<wlr_xdg_popup *>(data), scene_layer->tree);
	});
	output->layers[index].push_back(this);
}

Layer::~Layer()
{
	unfocus_layer(this);
	if (!output)
		return;
	for (std::vector<Layer *> &list : output->layers)
		erase(list, this);
	layers_arrange(output);
}

static void handle_new_layer_surface(void *data)
{
	auto *layer_surface = static_cast<wlr_layer_surface_v1 *>(data);
	if (!layer_surface->output && focused_output())
		layer_surface->output = focused_output()->wlr_output;
	if (!layer_surface->output || !layer_surface->output->data) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	new Layer(layer_surface, static_cast<Output *>(layer_surface->output->data));
}

/* ---- outputs ---- */

/* OUTPUT, already out of server.outputs, is going away or turned off:
 * close its layer surfaces and move its frames to another output, the
 * focused one shown there. */
static void output_leave(Output *output)
{
	for (std::vector<Layer *> &list : output->layers) {
		std::vector<Layer *> layers;
		layers.swap(list);
		for (Layer *layer : layers) {
			layer->output = nullptr;
			wlr_layer_surface_v1_destroy(layer->layer_surface);
		}
	}
	ipc_send("(output-removed :name %s)", lisp_string(output->wlr_output->name));
	bar_output_destroyed(output->wlr_output);

	/* its frames move to the first remaining output */
	Output *destination = server.outputs.empty() ? nullptr : server.outputs.front();
	View *focused = server.focused_view, *focused_frame = frame_of(focused);
	for (View *view : server.views) {
		if (!view->is_frame || view->output != output)
			continue;
		view->output = destination;
		if (destination && view->mapped &&
		    (!destination->active_frame || view == focused_frame))
			destination->active_frame = view;
	}
	output->active_frame = nullptr;
	if (focused && focused->is_frame && !frame_is_visible(focused))
		focus_fallback();
	else
		arrange();
}

/* Turn OUTPUT on, in its preferred mode, or off. */
static bool output_commit_enabled(Output *output, bool enabled)
{
	wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, enabled);
	if (enabled) {
		wlr_output_mode *mode = wlr_output_preferred_mode(output->wlr_output);
		if (mode)
			wlr_output_state_set_mode(&state, mode);
		const char *scale = getenv("WAYMACS_SCALE");
		if (scale && atof(scale) > 0)
			wlr_output_state_set_scale(&state, atof(scale));
	}
	bool committed = wlr_output_commit_state(output->wlr_output, &state);
	wlr_output_state_finish(&state);
	return committed;
}

/* Show OUTPUT: place it in the layout, render it, tell Emacs. */
static void output_show(Output *output)
{
	auto *wlr_output = output->wlr_output;
	output->enabled = true;
	server.outputs.push_back(output);
	wlr_scene_node_set_enabled(&output->background->node, true);
	wlr_scene_node_set_enabled(&output->lock_background->node, true);
	wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server.output_layout, wlr_output);
	output->scene_output = wlr_scene_output_create(server.scene, wlr_output);
	wlr_scene_output_layout_add_output(server.scene_layout, layout_output, output->scene_output);
	wlr_output_layout_get_box(server.output_layout, wlr_output, &output->box);
	output->usable_box = output->box;
	wlr_log(WLR_INFO, "output %s: %dx%d at %d,%d", wlr_output->name, output->box.width,
		output->box.height, output->box.x, output->box.y);
	/* frames left without a monitor (all were gone) come here */
	for (View *view : server.views) {
		if (!view->is_frame || view->output)
			continue;
		view->output = output;
		if (view->mapped && !output->active_frame)
			output->active_frame = view;
	}
	if (ipc_connected()) {
		outputs_announce();
		ipc_send("(need-frame :output %s)", lisp_string(wlr_output->name));
	}
	arrange();
}

Output::Output(struct wlr_output *output) : wlr_output(output)
{
	output->data = this;
	output_commit_enabled(this, true);
	render.listen(&output->events.frame, [this](void *) {
		wlr_scene_output_commit(scene_output, nullptr);
		timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_scene_output_send_frame_done(scene_output, &now);
	});
	/* nested backends: the host window was resized */
	request_state.listen(&output->events.request_state, [this](void *data) {
		auto *event = static_cast<wlr_output_event_request_state *>(data);
		wlr_output_commit_state(wlr_output, event->state);
	});
	destroy.listen(&output->events.destroy, [this](void *) { delete this; });

	static const float background_color[4] = { 0.16f, 0.16f, 0.18f, 1.0f };
	background = wlr_scene_rect_create(server.background_tree, 0, 0, background_color);
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	lock_background = wlr_scene_rect_create(server.lock_tree, 0, 0, black);
	output_show(this);
}

Output::~Output()
{
	erase(server.outputs, this);
	erase(server.disabled_outputs, this);
	wlr_scene_node_destroy(&background->node);
	wlr_scene_node_destroy(&lock_background->node);
	if (enabled)
		output_leave(this);
	wlr_output->data = nullptr;
	/* the last monitor was unplugged while another was off (lid closed):
	 * never leave the session without a screen */
	if (!server.quitting && server.outputs.empty() && !server.disabled_outputs.empty())
		output_set_enabled(server.disabled_outputs.front(), true);
}

/* Turn a monitor off (its workspaces move to another one) or on.  The
 * last monitor stays on. */
void output_set_enabled(Output *output, bool enabled)
{
	if (output->enabled == enabled)
		return;
	if (enabled) {
		erase(server.disabled_outputs, output);
		if (!output_commit_enabled(output, true)) {
			server.disabled_outputs.insert(server.disabled_outputs.begin(), output);
			return;
		}
		output_show(output);
		return;
	}
	if (server.outputs.size() < 2)
		return;
	output->enabled = false;
	erase(server.outputs, output);
	server.disabled_outputs.insert(server.disabled_outputs.begin(), output);
	wlr_scene_node_set_enabled(&output->background->node, false);
	wlr_scene_node_set_enabled(&output->lock_background->node, false);
	/* the scene output first: it is tied to the layout output */
	wlr_scene_output_destroy(output->scene_output);
	output->scene_output = nullptr;
	wlr_output_layout_remove(server.output_layout, output->wlr_output);
	output_commit_enabled(output, false);
	wlr_log(WLR_INFO, "output %s turned off", output->wlr_output->name);
	ipc_send("(output-off :name %s)", lisp_string(output->wlr_output->name));
	output_leave(output);
}

Output *output_by_name(const char *name, bool enabled_only)
{
	auto named = [name](Output *output) { return !strcmp(output->wlr_output->name, name); };
	auto found = std::find_if(server.outputs.begin(), server.outputs.end(), named);
	if (found != server.outputs.end())
		return *found;
	if (enabled_only)
		return nullptr;
	found = std::find_if(server.disabled_outputs.begin(), server.disabled_outputs.end(), named);
	return found != server.disabled_outputs.end() ? *found : nullptr;
}

void output_init()
{
	server.listen(&server.backend->events.new_output, [](void *data) {
		auto *wlr_output = static_cast<struct wlr_output *>(data);
		if (wlr_output_init_render(wlr_output, server.allocator, server.renderer))
			new Output(wlr_output);
	});
	server.listen(&server.output_layout->events.change, [](void *) {
		for (Output *output : server.outputs) {
			wlr_output_layout_get_box(server.output_layout, output->wlr_output, &output->box);
			layers_arrange(output);
		}
		outputs_announce();
		arrange();
	});
	server.listen(&wlr_layer_shell_v1_create(server.display, 4)->events.new_surface,
		      handle_new_layer_surface);
}
