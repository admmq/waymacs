/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* output.cpp - outputs and layer-shell surfaces (bars, wallpapers,
 * launchers, notifications). */
#include "waymacs.h"

void outputs_announce(void)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link)
		ipc_send("(output :name %s :x %d :y %d :width %d :height %d :scale %.3f)",
			 lisp_string(output->wlr_output->name), output->box.x, output->box.y,
			 output->box.width, output->box.height, output->wlr_output->scale);
}

/* ---- layer shell ---- */

/* Configure OUTPUT's layer surfaces that have (EXCLUSIVE) or have not an
 * exclusive zone; the zones are taken out of USABLE_BOX. */
static void configure_layers(struct output *output, bool exclusive, struct wlr_box *usable_box)
{
	for (int layer_index = 3; layer_index >= 0; layer_index--) {
		struct layer *layer;
		wl_list_for_each(layer, &output->layers[layer_index], link) {
			struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
			if (layer_surface->initialized &&
			    (layer_surface->current.exclusive_zone > 0) == exclusive)
				wlr_scene_layer_surface_v1_configure(layer->scene_layer, &output->box,
								     usable_box);
		}
	}
}

/* the topmost top/overlay surface of OUTPUT that wants the keyboard for itself */
static struct layer *exclusive_keyboard_layer(struct output *output)
{
	for (int layer_index = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
	     layer_index >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; layer_index--) {
		struct layer *layer;
		wl_list_for_each_reverse(layer, &output->layers[layer_index], link)
			if (layer->mapped && layer->layer_surface->current.keyboard_interactive ==
			    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
				return layer;
	}
	return NULL;
}

void layers_arrange(struct output *output)
{
	struct wlr_box usable_box = output->box;
	configure_layers(output, true, &usable_box);
	configure_layers(output, false, &usable_box);
	bool usable_box_changed = memcmp(&usable_box, &output->usable_box, sizeof usable_box) != 0;
	output->usable_box = usable_box;

	struct layer *keyboard_layer = exclusive_keyboard_layer(output);
	if (keyboard_layer && server.focused_layer != keyboard_layer) {
		server.focused_layer = keyboard_layer;
		apply_keyboard_focus(UINT32_MAX);
	}
	if (usable_box_changed)
		arrange();	/* Emacs frames are resized to the usable box */
}

static void handle_layer_commit(struct wl_listener *listener, void *data)
{
	struct layer *layer = wl_container_of(listener, layer, commit);
	struct wlr_layer_surface_v1 *layer_surface = layer->layer_surface;
	if (!layer->output)
		return;
	if (layer_surface->initial_commit) {
		/* arrange with the pending state so the first configure fits */
		struct wlr_layer_surface_v1_state old_state = layer_surface->current;
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
		enum zwlr_layer_shell_v1_layer layer_index = layer_surface->current.layer;
		wlr_scene_node_reparent(&layer->scene_layer->tree->node,
					server.layer_trees[layer_index]);
		wl_list_remove(&layer->link);
		wl_list_insert(layer->output->layers[layer_index].prev, &layer->link);
	}
	layers_arrange(layer->output);
	/* launchers and the like want the keyboard as soon as they appear */
	if (just_mapped && layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
	    layer_surface->current.keyboard_interactive !=
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		server.focused_layer = layer;
		apply_keyboard_focus(UINT32_MAX);
	}
}

static void unfocus_layer(struct layer *layer)
{
	if (server.focused_layer != layer)
		return;
	server.focused_layer = NULL;
	apply_keyboard_focus(UINT32_MAX);
}

static void handle_layer_unmap(struct wl_listener *listener, void *data)
{
	struct layer *layer = wl_container_of(listener, layer, unmap);
	layer->mapped = false;
	unfocus_layer(layer);
	if (layer->output)
		layers_arrange(layer->output);
}

static void handle_layer_destroy(struct wl_listener *listener, void *data)
{
	struct layer *layer = wl_container_of(listener, layer, destroy);
	struct output *output = layer->output;
	unfocus_layer(layer);
	wl_list_remove(&layer->link);
	stop_listening(&layer->commit, &layer->unmap, &layer->destroy, &layer->new_popup);
	free(layer);
	if (output)
		layers_arrange(output);
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data)
{
	auto *layer_surface = (struct wlr_layer_surface_v1 *)data;
	if (!layer_surface->output && focused_output())
		layer_surface->output = focused_output()->wlr_output;
	struct output *output = layer_surface->output ?
		(struct output *)layer_surface->output->data : NULL;
	if (!output) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}
	enum zwlr_layer_shell_v1_layer layer_index = layer_surface->pending.layer;
	struct layer *layer = (struct layer *)calloc(1, sizeof *layer);
	layer->type = OWNER_LAYER;
	layer->layer_surface = layer_surface;
	layer->output = output;
	layer->scene_layer = wlr_scene_layer_surface_v1_create(server.layer_trees[layer_index],
							       layer_surface);
	layer->scene_layer->tree->node.data = layer;
	layer_surface->surface->data = layer->scene_layer->tree;	/* popups attach here */
	layer_surface->data = layer;
	LISTEN(&layer_surface->surface->events.commit, &layer->commit, handle_layer_commit);
	LISTEN(&layer_surface->surface->events.unmap, &layer->unmap, handle_layer_unmap);
	LISTEN(&layer_surface->events.destroy, &layer->destroy, handle_layer_destroy);
	LISTEN(&layer_surface->events.new_popup, &layer->new_popup,
	       [](struct wl_listener *listener, void *data) {
		struct layer *layer = OWNER(listener, struct layer, new_popup);
		popup_create((struct wlr_xdg_popup *)data, layer->scene_layer->tree);
	});
	wl_list_insert(output->layers[layer_index].prev, &layer->link);
}

/* ---- outputs ---- */

static void handle_output_render(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, render);
	wlr_scene_output_commit(output->scene_output, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void handle_output_request_state(struct wl_listener *listener, void *data)
{
	/* nested backends: the host window was resized */
	struct output *output = wl_container_of(listener, output, request_state);
	auto *event = (struct wlr_output_event_request_state *)data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

/* OUTPUT, already out of server.outputs, is going away or turned off:
 * close its layer surfaces and move its frames to another output, the
 * focused one shown there. */
static void output_leave(struct output *output)
{
	for (int layer_index = 0; layer_index < 4; layer_index++) {
		struct layer *layer, *next_layer;
		wl_list_for_each_safe(layer, next_layer, &output->layers[layer_index], link) {
			layer->output = NULL;
			wl_list_remove(&layer->link);
			wl_list_init(&layer->link);
			wlr_layer_surface_v1_destroy(layer->layer_surface);
		}
	}
	ipc_send("(output-removed :name %s)", lisp_string(output->wlr_output->name));
	bar_output_destroyed(output->wlr_output);

	/* its frames move to the first remaining output */
	struct output *destination = NULL;
	if (!wl_list_empty(&server.outputs))
		destination = wl_container_of(server.outputs.next, destination, link);
	struct view *focused = server.focused_view, *view;
	struct view *focused_frame = focused && !focused->is_frame ? focused->frame : focused;
	wl_list_for_each(view, &server.views, link) {
		if (!view->is_frame || view->output != output)
			continue;
		view->output = destination;
		if (destination && view->mapped &&
		    (!destination->active_frame || view == focused_frame))
			destination->active_frame = view;
	}
	output->active_frame = NULL;
	if (focused && focused->is_frame && !frame_is_visible(focused))
		focus_fallback();
	else
		arrange();
}

static void handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, destroy);
	stop_listening(&output->render, &output->request_state, &output->destroy);
	wl_list_remove(&output->link);
	wlr_scene_node_destroy(&output->background->node);
	wlr_scene_node_destroy(&output->lock_background->node);
	if (output->enabled)
		output_leave(output);
	free(output);
	/* the last monitor was unplugged while another was off (lid closed):
	 * never leave the session without a screen */
	if (!server.quitting && wl_list_empty(&server.outputs) &&
	    !wl_list_empty(&server.disabled_outputs)) {
		struct output *disabled_output =
			wl_container_of(server.disabled_outputs.next, disabled_output, link);
		output_set_enabled(disabled_output, true);
	}
}

/* Turn OUTPUT on, in its preferred mode. */
static bool output_commit_enabled(struct output *output, bool enabled)
{
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, enabled);
	if (enabled) {
		struct wlr_output_mode *mode = wlr_output_preferred_mode(output->wlr_output);
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
static void output_show(struct output *output)
{
	struct wlr_output *wlr_output = output->wlr_output;
	output->enabled = true;
	wl_list_insert(server.outputs.prev, &output->link);
	wlr_scene_node_set_enabled(&output->background->node, true);
	wlr_scene_node_set_enabled(&output->lock_background->node, true);
	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server.output_layout, wlr_output);
	output->scene_output = wlr_scene_output_create(server.scene, wlr_output);
	wlr_scene_output_layout_add_output(server.scene_layout, layout_output, output->scene_output);
	wlr_output_layout_get_box(server.output_layout, wlr_output, &output->box);
	output->usable_box = output->box;
	wlr_log(WLR_INFO, "output %s: %dx%d at %d,%d", wlr_output->name, output->box.width,
		output->box.height, output->box.x, output->box.y);
	/* frames left without a monitor (all were gone) come here */
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
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

/* Turn a monitor off (its workspaces move to another one) or on.  The
 * last monitor stays on. */
void output_set_enabled(struct output *output, bool enabled)
{
	if (output->enabled == enabled)
		return;
	if (enabled) {
		wl_list_remove(&output->link);
		if (!output_commit_enabled(output, true)) {
			wl_list_insert(&server.disabled_outputs, &output->link);
			return;
		}
		output_show(output);
		return;
	}
	if (wl_list_length(&server.outputs) < 2)
		return;
	output->enabled = false;
	wl_list_remove(&output->link);
	wl_list_insert(&server.disabled_outputs, &output->link);
	wlr_scene_node_set_enabled(&output->background->node, false);
	wlr_scene_node_set_enabled(&output->lock_background->node, false);
	/* the scene output first: it is tied to the layout output */
	wlr_scene_output_destroy(output->scene_output);
	output->scene_output = NULL;
	wlr_output_layout_remove(server.output_layout, output->wlr_output);
	output_commit_enabled(output, false);
	wlr_log(WLR_INFO, "output %s turned off", output->wlr_output->name);
	ipc_send("(output-off :name %s)", lisp_string(output->wlr_output->name));
	output_leave(output);
}

struct output *output_by_name(const char *name, bool enabled_only)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link)
		if (!strcmp(output->wlr_output->name, name))
			return output;
	if (!enabled_only)
		wl_list_for_each(output, &server.disabled_outputs, link)
			if (!strcmp(output->wlr_output->name, name))
				return output;
	return NULL;
}

static void handle_new_output(struct wl_listener *listener, void *data)
{
	auto *wlr_output = (struct wlr_output *)data;
	if (!wlr_output_init_render(wlr_output, server.allocator, server.renderer))
		return;
	struct output *output = (struct output *)calloc(1, sizeof *output);
	output->wlr_output = wlr_output;
	wlr_output->data = output;
	for (int layer_index = 0; layer_index < 4; layer_index++)
		wl_list_init(&output->layers[layer_index]);

	output_commit_enabled(output, true);

	LISTEN(&wlr_output->events.frame, &output->render, handle_output_render);
	LISTEN(&wlr_output->events.request_state, &output->request_state,
	       handle_output_request_state);
	LISTEN(&wlr_output->events.destroy, &output->destroy, handle_output_destroy);
	wl_list_init(&output->link);	/* output_show() moves it to server.outputs */

	static const float background_color[4] = { 0.16f, 0.16f, 0.18f, 1.0f };
	output->background = wlr_scene_rect_create(server.background_tree, 0, 0, background_color);
	static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	output->lock_background = wlr_scene_rect_create(server.lock_tree, 0, 0, black);
	output_show(output);
}

static void handle_output_layout_change(struct wl_listener *listener, void *data)
{
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		wlr_output_layout_get_box(server.output_layout, output->wlr_output, &output->box);
		layers_arrange(output);
	}
	outputs_announce();
	arrange();
}

void output_init(void)
{
	LISTEN(&server.backend->events.new_output, &server.new_output, handle_new_output);
	LISTEN(&server.output_layout->events.change, &server.output_layout_change,
	       handle_output_layout_change);
	server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
	LISTEN(&server.layer_shell->events.new_surface, &server.new_layer_surface,
	       handle_new_layer_surface);
}
