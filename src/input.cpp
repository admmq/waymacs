/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* input.cpp - keyboards (prefix keys, simulation keys), the pointer,
 * interactive move/resize, seat requests. */
#include "waymacs.h"
#include <linux/input-event-codes.h>

static uint32_t now_msec(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* ---- keyboards ---- */

static void set_keymap(struct wlr_keyboard *wlr_keyboard)
{
	struct xkb_rule_names names;
	memset(&names, 0, sizeof names);
	names.layout = server.xkb_layout[0] ? server.xkb_layout : NULL;
	names.variant = server.xkb_variant[0] ? server.xkb_variant : NULL;
	names.options = server.xkb_options[0] ? server.xkb_options : NULL;
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names,
							      XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap)
		wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	else
		wlr_log(WLR_ERROR, "bad xkb settings: layout `%s' variant `%s' options `%s'",
			server.xkb_layout, server.xkb_variant, server.xkb_options);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

void keyboards_reconfigure(void)
{
	struct keyboard *keyboard;
	wl_list_for_each(keyboard, &server.keyboards, link) {
		if (!keyboard->is_virtual)
			set_keymap(keyboard->wlr_keyboard);
		wlr_keyboard_set_repeat_info(keyboard->wlr_keyboard, server.repeat_rate,
					     server.repeat_delay);
	}
	set_keymap(&server.inject_keyboard);
}

/* The (keysym, modifiers) pairs a key press matches: the translated keysym
 * without the modifiers it consumed (M-% is Alt+Shift+5), and the
 * unshifted keysym with all modifiers (C-S-x, non-latin layouts). */
struct key_candidate {
	xkb_keysym_t keysym;
	uint32_t modifiers;
};

static int key_candidates(struct wlr_keyboard *wlr_keyboard, xkb_keycode_t keycode,
			  struct key_candidate *candidates)
{
	struct xkb_state *state = wlr_keyboard->xkb_state;
	uint32_t modifiers = wlr_keyboard_get_modifiers(wlr_keyboard) & MODIFIER_MASK;
	uint32_t consumed = 0;
	for (int i = 0; i < WLR_MODIFIER_COUNT; i++) {
		xkb_mod_index_t index = wlr_keyboard->mod_indexes[i];
		if (index != XKB_MOD_INVALID &&
		    xkb_state_mod_index_is_consumed2(state, keycode, index,
						     XKB_CONSUMED_MODE_XKB) == 1)
			consumed |= 1u << i;
	}
	int count = 0;
	const xkb_keysym_t *keysyms;
	int keysym_count = xkb_state_key_get_syms(state, keycode, &keysyms);
	for (int i = 0; i < keysym_count && count < 8; i++, count++) {
		candidates[count].keysym = xkb_keysym_to_lower(keysyms[i]);
		candidates[count].modifiers = modifiers & ~consumed;
	}
	/* level one of the active layout, then of the first layout */
	xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
	for (xkb_layout_index_t current = layout;; current = 0) {
		keysym_count = xkb_keymap_key_get_syms_by_level(wlr_keyboard->keymap, keycode,
								current, 0, &keysyms);
		for (int i = 0; i < keysym_count && count < 8; i++, count++) {
			candidates[count].keysym = xkb_keysym_to_lower(keysyms[i]);
			candidates[count].modifiers = modifiers;
		}
		if (current == 0)
			break;
	}
	return count;
}

static bool key_matches(const struct key_spec *key, const struct key_candidate *candidates,
			int count)
{
	for (int i = 0; i < count; i++)
		if (key->keysym == candidates[i].keysym && key->modifiers == candidates[i].modifiers)
			return true;
	return false;
}

static bool any_key_matches(const struct key_spec *keys, int key_count,
			    const struct key_candidate *candidates, int count)
{
	for (int i = 0; i < key_count; i++)
		if (key_matches(&keys[i], candidates, count))
			return true;
	return false;
}

/* The keycode typing KEYSYM, and whether Shift is needed for it. */
static bool find_keycode(struct wlr_keyboard *wlr_keyboard, xkb_keysym_t keysym,
			 xkb_keycode_t *keycode, uint32_t *shift)
{
	struct xkb_keymap *keymap = wlr_keyboard->keymap;
	xkb_layout_index_t layout = xkb_state_serialize_layout(wlr_keyboard->xkb_state,
							       XKB_STATE_LAYOUT_EFFECTIVE);
	for (xkb_level_index_t level = 0; level < 2; level++) {
		for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap);
		     code <= xkb_keymap_max_keycode(keymap); code++) {
			const xkb_keysym_t *keysyms;
			int count = xkb_keymap_key_get_syms_by_level(keymap, code, layout, level,
								     &keysyms);
			for (int i = 0; i < count; i++) {
				if (keysyms[i] == keysym) {
					*keycode = code;
					*shift = level ? WLR_MODIFIER_SHIFT : 0;
					return true;
				}
			}
		}
	}
	return false;
}

static bool can_type(struct wlr_keyboard *wlr_keyboard, const struct key_spec *keys, int count)
{
	xkb_keycode_t keycode;
	uint32_t shift;
	if (!wlr_keyboard || !wlr_keyboard->xkb_state)
		return false;
	for (int i = 0; i < count; i++)
		if (!find_keycode(wlr_keyboard, keys[i].keysym, &keycode, &shift))
			return false;
	return true;
}

static xkb_mod_mask_t xkb_modifier_mask(struct wlr_keyboard *wlr_keyboard, uint32_t modifiers)
{
	xkb_mod_mask_t mask = 0;
	for (int i = 0; i < WLR_MODIFIER_COUNT; i++)
		if ((modifiers & (1u << i)) && wlr_keyboard->mod_indexes[i] != XKB_MOD_INVALID)
			mask |= 1u << wlr_keyboard->mod_indexes[i];
	return mask;
}

/* Type KEYS in the focused client.  With HOLD_LAST the last key stays
 * down (so the client's own key repeat works); its keycode is returned. */
static uint32_t send_keys(const struct key_spec *keys, int count, bool hold_last)
{
	/* virtual keyboards (wtype...) have minimal keymaps: type with our
	 * own keyboard when the active one cannot */
	struct wlr_keyboard *previous = wlr_seat_get_keyboard(server.seat);
	struct wlr_keyboard *wlr_keyboard = previous;
	if (!can_type(wlr_keyboard, keys, count)) {
		wlr_keyboard = &server.inject_keyboard;
		wlr_seat_set_keyboard(server.seat, wlr_keyboard);
	}
	if (!wlr_keyboard->xkb_state)
		return 0;
	struct wlr_keyboard_modifiers saved_modifiers = (previous ? previous : wlr_keyboard)->modifiers;
	xkb_layout_index_t layout = xkb_state_serialize_layout(wlr_keyboard->xkb_state,
							       XKB_STATE_LAYOUT_EFFECTIVE);
	uint32_t time_msec = now_msec(), held_keycode = 0;
	for (int i = 0; i < count; i++) {
		xkb_keycode_t keycode;
		uint32_t shift;
		if (!find_keycode(wlr_keyboard, keys[i].keysym, &keycode, &shift)) {
			char name[64];
			xkb_keysym_get_name(keys[i].keysym, name, sizeof name);
			wlr_log(WLR_INFO, "no key in the keymap types %s", name);
			continue;
		}
		struct wlr_keyboard_modifiers modifiers;
		memset(&modifiers, 0, sizeof modifiers);
		modifiers.depressed = xkb_modifier_mask(wlr_keyboard, keys[i].modifiers | shift);
		modifiers.group = layout;
		wlr_seat_keyboard_notify_modifiers(server.seat, &modifiers);
		/* the seat speaks evdev keycodes, xkb adds 8 */
		wlr_seat_keyboard_notify_key(server.seat, time_msec, keycode - 8,
					     WL_KEYBOARD_KEY_STATE_PRESSED);
		if (hold_last && i == count - 1) {
			held_keycode = keycode - 8;
			break;
		}
		wlr_seat_keyboard_notify_key(server.seat, time_msec, keycode - 8,
					     WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	if (!held_keycode) {
		if (previous && previous != wlr_keyboard)
			wlr_seat_set_keyboard(server.seat, previous);
		wlr_seat_keyboard_notify_modifiers(server.seat, &saved_modifiers);
	}
	return held_keycode;
}

void inject_keys(const struct key_spec *keys, int count)
{
	send_keys(keys, count, false);
}

static void swallow_release(struct keyboard *keyboard, uint32_t keycode)
{
	for (int i = 0; i < 16; i++) {
		if (!keyboard->swallowed_keycodes[i]) {
			keyboard->swallowed_keycodes[i] = keycode;
			return;
		}
	}
}

static bool release_is_swallowed(struct keyboard *keyboard, uint32_t keycode)
{
	for (int i = 0; i < 16; i++) {
		if (keyboard->swallowed_keycodes[i] == keycode) {
			keyboard->swallowed_keycodes[i] = 0;
			return true;
		}
	}
	return false;
}

/* Keys the compositor always handles: VT switching, emergency exit. */
static bool handle_compositor_key(const struct key_candidate *candidates, int count)
{
	const uint32_t emergency_exit = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT;
	for (int i = 0; i < count; i++) {
		xkb_keysym_t keysym = candidates[i].keysym;
		if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
			if (server.session)
				wlr_session_change_vt(server.session,
						      keysym - XKB_KEY_XF86Switch_VT_1 + 1);
			return true;
		}
		/* not while locked: anyone could end the session */
		if (keysym == XKB_KEY_BackSpace && candidates[i].modifiers == emergency_exit &&
		    !server.locked) {
			wlr_log(WLR_INFO, "Ctrl+Alt+Shift+BackSpace: quitting");
			wl_display_terminate(server.display);
			return true;
		}
	}
	return false;
}

/* The application holding the keyboard, if any. */
static struct view *focused_app(void)
{
	struct view *view = server.focused_view;
	if (server.focused_layer || !view || view->is_frame || !view->mapped ||
	    server.seat->keyboard_state.focused_surface != view_surface(view))
		return NULL;
	return view;
}

static void handle_key(struct wl_listener *listener, void *data)
{
	struct keyboard *keyboard = wl_container_of(listener, keyboard, key);
	auto *event = (struct wlr_keyboard_key_event *)data;
	struct wlr_keyboard *wlr_keyboard = keyboard->wlr_keyboard;
	wlr_seat_set_keyboard(server.seat, wlr_keyboard);
	if (event->time_msec == 0)	/* some virtual keyboards send no time */
		event->time_msec = now_msec();

	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (keyboard->held_physical_keycode &&
		    keyboard->held_physical_keycode == event->keycode) {
			wlr_seat_keyboard_notify_key(server.seat, event->time_msec,
						     keyboard->held_injected_keycode, event->state);
			wlr_seat_keyboard_notify_modifiers(server.seat, &wlr_keyboard->modifiers);
			keyboard->held_physical_keycode = 0;
		} else if (!release_is_swallowed(keyboard, event->keycode)) {
			wlr_seat_keyboard_notify_key(server.seat, event->time_msec,
						     event->keycode, event->state);
		}
		return;
	}
	if (wlr_keyboard->xkb_state) {
		struct key_candidate candidates[8];
		int count = key_candidates(wlr_keyboard, event->keycode + 8, candidates);
		if (handle_compositor_key(candidates, count)) {
			swallow_release(keyboard, event->keycode);
			return;
		}
		if (server.locked) {	/* every other key goes to the locker */
			wlr_seat_keyboard_notify_key(server.seat, event->time_msec,
						     event->keycode, event->state);
			return;
		}
		struct view *app = focused_app();
		bool line_mode = app && !app->char_mode && !app->fullscreen;
		if (app && (any_key_matches(server.global_keys, server.global_key_count,
					    candidates, count) ||
			    (line_mode && any_key_matches(server.prefix_keys,
							  server.prefix_key_count,
							  candidates, count)))) {
			/* Give the keyboard to the Emacs frame and replay the
			 * press there; Emacs hands it back when it is done. */
			struct view *frame = frame_is_visible(app->frame) ? app->frame : NULL;
			if (!frame && focused_output())
				frame = focused_output()->active_frame;
			if (frame) {
				focus_frame_for_key(frame, event->keycode);
				wlr_seat_keyboard_notify_key(server.seat, event->time_msec,
							     event->keycode, event->state);
				return;
			}
		}
		for (int i = 0; line_mode && i < server.simulation_key_count; i++) {
			struct simulation_key *simulation = &server.simulation_keys[i];
			if (!key_matches(&simulation->from, candidates, count))
				continue;
			uint32_t held = send_keys(simulation->to, simulation->count, true);
			if (held && !keyboard->held_physical_keycode) {
				keyboard->held_physical_keycode = event->keycode;
				keyboard->held_injected_keycode = held;
			} else {
				swallow_release(keyboard, event->keycode);
			}
			return;
		}
	}
	wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
}

static void handle_modifiers(struct wl_listener *listener, void *data)
{
	struct keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(server.seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(server.seat, &keyboard->wlr_keyboard->modifiers);
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data)
{
	struct keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	stop_listening(&keyboard->modifiers, &keyboard->key, &keyboard->destroy);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void keyboard_add(struct wlr_keyboard *wlr_keyboard, bool is_virtual)
{
	struct keyboard *keyboard = (struct keyboard *)calloc(1, sizeof *keyboard);
	keyboard->wlr_keyboard = wlr_keyboard;
	keyboard->is_virtual = is_virtual;
	if (!is_virtual)	/* virtual keyboards bring their own keymap */
		set_keymap(wlr_keyboard);
	wlr_keyboard_set_repeat_info(wlr_keyboard, server.repeat_rate, server.repeat_delay);
	LISTEN(&wlr_keyboard->events.modifiers, &keyboard->modifiers, handle_modifiers);
	LISTEN(&wlr_keyboard->events.key, &keyboard->key, handle_key);
	LISTEN(&wlr_keyboard->base.events.destroy, &keyboard->destroy, handle_keyboard_destroy);
	wl_list_insert(&server.keyboards, &keyboard->link);
	/* A virtual keyboard has no keymap until its client sends one; made
	 * the seat's keyboard now, clients would be told "no keymap", which
	 * some (swaylock) treat as fatal.  Its first key event selects it. */
	if (wlr_keyboard->keymap)
		wlr_seat_set_keyboard(server.seat, wlr_keyboard);
}

/* ---- pointer ---- */

/* The surface at layout coordinates X, Y and the view or layer it belongs to. */
static struct wlr_surface *surface_at(double x, double y, double *surface_x, double *surface_y,
				      struct view **view, struct layer **layer)
{
	*view = NULL;
	*layer = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(&server.scene->tree.node, x, y,
							surface_x, surface_y);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	if (!scene_surface)
		return NULL;
	for (struct wlr_scene_tree *tree = node->parent; tree; tree = tree->node.parent) {
		if (!tree->node.data)
			continue;
		int owner = *(int *)tree->node.data;
		if (owner == OWNER_VIEW)
			*view = (struct view *)tree->node.data;
		else if (owner == OWNER_LAYER)
			*layer = (struct layer *)tree->node.data;
		break;
	}
	return scene_surface->surface;
}

void begin_grab(struct view *view, enum cursor_mode mode, uint32_t edges)
{
	if (server.cursor_mode != CURSOR_PASSTHROUGH)
		return;
	server.cursor_mode = mode;
	server.grabbed_view = view;
	server.grab_edges = edges ? edges : WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
	server.grab_box = view->floating_box;
	server.grab_cursor_x = server.cursor->x;
	server.grab_cursor_y = server.cursor->y;
}

static void grab_motion(void)
{
	int delta_x = (int)(server.cursor->x - server.grab_cursor_x);
	int delta_y = (int)(server.cursor->y - server.grab_cursor_y);
	struct wlr_box box = server.grab_box;
	if (server.cursor_mode == CURSOR_MOVE) {
		box.x += delta_x;
		box.y += delta_y;
	} else {
		if (server.grab_edges & WLR_EDGE_LEFT) {
			box.x += delta_x;
			box.width -= delta_x;
		} else if (server.grab_edges & WLR_EDGE_RIGHT) {
			box.width += delta_x;
		}
		if (server.grab_edges & WLR_EDGE_TOP) {
			box.y += delta_y;
			box.height -= delta_y;
		} else if (server.grab_edges & WLR_EDGE_BOTTOM) {
			box.height += delta_y;
		}
		if (box.width < 64 || box.height < 64)
			return;
	}
	server.grabbed_view->floating_box = box;
	arrange();
}

static void process_motion(uint32_t time_msec)
{
	if (server.cursor_mode != CURSOR_PASSTHROUGH && server.grabbed_view) {
		grab_motion();
		return;
	}
	double surface_x, surface_y;
	struct view *view;
	struct layer *layer;
	struct wlr_surface *surface = surface_at(server.cursor->x, server.cursor->y,
						 &surface_x, &surface_y, &view, &layer);
	if (server.locked && !is_lock_surface(surface))
		surface = NULL;
	if (!surface) {
		wlr_cursor_set_xcursor(server.cursor, server.xcursor_manager, "default");
		wlr_seat_pointer_clear_focus(server.seat);
		return;
	}
	wlr_seat_pointer_notify_enter(server.seat, surface, surface_x, surface_y);
	if (time_msec)
		wlr_seat_pointer_notify_motion(server.seat, time_msec, surface_x, surface_y);
}

/* after windows moved: the pointer may be over another surface now */
void update_pointer_focus(void)
{
	if (server.cursor_mode == CURSOR_PASSTHROUGH)
		process_motion(0);
}

/* Click to focus.  Emacs learns about it with an `activate' event. */
static void focus_clicked(struct view *view, struct layer *layer)
{
	if (layer) {
		if (layer->layer_surface->current.keyboard_interactive !=
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			server.focused_layer = layer;
			apply_keyboard_focus(UINT32_MAX);
		}
		return;
	}
	if (server.focused_layer &&
	    server.focused_layer->layer_surface->current.keyboard_interactive !=
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
		server.focused_layer = NULL;
	if (view && view != server.focused_view) {
		focus_view(view, false);
		ipc_send("(activate :id %u)", view->id);
	} else {
		apply_keyboard_focus(UINT32_MAX);
	}
}

static void handle_cursor_button(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_pointer_button_event *)data;
	if (server.locked) {	/* only to the locker, focuses nothing */
		wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button,
					       event->state);
		return;
	}
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double surface_x, surface_y;
		struct view *view;
		struct layer *layer;
		surface_at(server.cursor->x, server.cursor->y, &surface_x, &surface_y,
			   &view, &layer);
		struct wlr_keyboard *wlr_keyboard = wlr_seat_get_keyboard(server.seat);
		uint32_t modifiers = wlr_keyboard ? wlr_keyboard_get_modifiers(wlr_keyboard) : 0;
		focus_clicked(view, layer);
		/* Super + left/right drag moves/resizes floating windows */
		if (view && !view->is_frame && view->floating && !view->fullscreen &&
		    (modifiers & WLR_MODIFIER_LOGO) &&
		    (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
			begin_grab(view, event->button == BTN_LEFT ? CURSOR_MOVE : CURSOR_RESIZE, 0);
			return;
		}
	} else if (server.cursor_mode != CURSOR_PASSTHROUGH) {
		server.cursor_mode = CURSOR_PASSTHROUGH;
		server.grabbed_view = NULL;
	}
	wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button, event->state);
}

static void handle_cursor_motion(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_pointer_motion_event *)data;
	wlr_cursor_move(server.cursor, &event->pointer->base, event->delta_x, event->delta_y);
	process_motion(event->time_msec);
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_pointer_motion_absolute_event *)data;
	wlr_cursor_warp_absolute(server.cursor, &event->pointer->base, event->x, event->y);
	process_motion(event->time_msec);
}

static void handle_cursor_axis(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_pointer_axis_event *)data;
	wlr_seat_pointer_notify_axis(server.seat, event->time_msec, event->orientation,
				     event->delta, event->delta_discrete, event->source,
				     event->relative_direction);
}

/* libinput leaves tap-to-click off; most people want it on touchpads */
static void configure_pointer(struct wlr_input_device *device)
{
	if (!wlr_input_device_is_libinput(device))
		return;
	struct libinput_device *libinput_device = wlr_libinput_get_device_handle(device);
	if (libinput_device_config_tap_get_finger_count(libinput_device) > 0)
		libinput_device_config_tap_set_enabled(libinput_device, LIBINPUT_CONFIG_TAP_ENABLED);
}

/* ---- the laptop lid: Emacs decides what to do about it ---- */

struct lid_switch {
	struct wl_listener toggle, destroy;
};

static void handle_switch_toggle(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_switch_toggle_event *)data;
	if (event->switch_type != WLR_SWITCH_TYPE_LID)
		return;
	bool closed = event->switch_state == WLR_SWITCH_STATE_ON;
	if (closed == server.lid_closed)
		return;
	server.lid_closed = closed;
	wlr_log(WLR_INFO, "lid %s", closed ? "closed" : "opened");
	ipc_send("(lid :closed %s)", closed ? "t" : "nil");
}

static void handle_switch_destroy(struct wl_listener *listener, void *data)
{
	struct lid_switch *lid_switch = wl_container_of(listener, lid_switch, destroy);
	stop_listening(&lid_switch->toggle, &lid_switch->destroy);
	free(lid_switch);
}

static void switch_add(struct wlr_input_device *device)
{
	struct lid_switch *lid_switch = (struct lid_switch *)calloc(1, sizeof *lid_switch);
	LISTEN(&wlr_switch_from_input_device(device)->events.toggle, &lid_switch->toggle,
	       handle_switch_toggle);
	LISTEN(&device->events.destroy, &lid_switch->destroy, handle_switch_destroy);
}

/* ---- devices and seat requests ---- */

static void handle_new_input(struct wl_listener *listener, void *data)
{
	auto *device = (struct wlr_input_device *)data;
	if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		keyboard_add(wlr_keyboard_from_input_device(device), false);
	} else if (device->type == WLR_INPUT_DEVICE_POINTER) {
		configure_pointer(device);
		wlr_cursor_attach_input_device(server.cursor, device);
	} else if (device->type == WLR_INPUT_DEVICE_SWITCH) {
		switch_add(device);
	}
}

static void handle_request_cursor(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_seat_pointer_request_set_cursor_event *)data;
	if (server.seat->pointer_state.focused_client == event->seat_client)
		wlr_cursor_set_surface(server.cursor, event->surface, event->hotspot_x,
				       event->hotspot_y);
}

static void handle_request_cursor_shape(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_cursor_shape_manager_v1_request_set_shape_event *)data;
	if (server.seat->pointer_state.focused_client == event->seat_client)
		wlr_cursor_set_xcursor(server.cursor, server.xcursor_manager,
				       wlr_cursor_shape_v1_name(event->shape));
}

static void handle_request_start_drag(struct wl_listener *listener, void *data)
{
	auto *event = (struct wlr_seat_request_start_drag_event *)data;
	if (wlr_seat_validate_pointer_grab_serial(server.seat, event->origin, event->serial))
		wlr_seat_start_pointer_drag(server.seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void input_init(void)
{
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	const char *cursor_size = getenv("XCURSOR_SIZE");
	server.xcursor_manager = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"),
		cursor_size && atoi(cursor_size) > 0 ? atoi(cursor_size) : 24);
	LISTEN(&server.cursor->events.motion, &server.cursor_motion, handle_cursor_motion);
	LISTEN(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute,
	       handle_cursor_motion_absolute);
	LISTEN(&server.cursor->events.button, &server.cursor_button, handle_cursor_button);
	LISTEN(&server.cursor->events.axis, &server.cursor_axis, handle_cursor_axis);
	LISTEN(&server.cursor->events.frame, &server.cursor_frame,
	       [](struct wl_listener *listener, void *data) {
		wlr_seat_pointer_notify_frame(server.seat);
	});

	static const struct wlr_keyboard_impl inject_keyboard_impl = { "waymacs-inject", NULL };
	wlr_keyboard_init(&server.inject_keyboard, &inject_keyboard_impl, "waymacs-inject");
	set_keymap(&server.inject_keyboard);

	server.seat = wlr_seat_create(server.display, "seat0");
	wlr_seat_set_capabilities(server.seat,
				  WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	LISTEN(&server.seat->events.request_set_cursor, &server.request_cursor,
	       handle_request_cursor);
	LISTEN(&server.seat->events.request_set_selection, &server.request_selection,
	       [](struct wl_listener *listener, void *data) {
		auto *event = (struct wlr_seat_request_set_selection_event *)data;
		wlr_seat_set_selection(server.seat, event->source, event->serial);
	});
	LISTEN(&server.seat->events.request_set_primary_selection,
	       &server.request_primary_selection, [](struct wl_listener *listener, void *data) {
		auto *event = (struct wlr_seat_request_set_primary_selection_event *)data;
		wlr_seat_set_primary_selection(server.seat, event->source, event->serial);
	});
	LISTEN(&server.seat->events.request_start_drag, &server.request_start_drag,
	       handle_request_start_drag);

	LISTEN(&server.backend->events.new_input, &server.new_input, handle_new_input);
	LISTEN(&wlr_virtual_keyboard_manager_v1_create(server.display)->events.new_virtual_keyboard,
	       &server.new_virtual_keyboard, [](struct wl_listener *listener, void *data) {
		keyboard_add(&((struct wlr_virtual_keyboard_v1 *)data)->keyboard, true);
	});
	LISTEN(&wlr_virtual_pointer_manager_v1_create(server.display)->events.new_virtual_pointer,
	       &server.new_virtual_pointer, [](struct wl_listener *listener, void *data) {
		auto *event = (struct wlr_virtual_pointer_v1_new_pointer_event *)data;
		wlr_cursor_attach_input_device(server.cursor, &event->new_pointer->pointer.base);
	});
	LISTEN(&wlr_cursor_shape_manager_v1_create(server.display, 1)->events.request_set_shape,
	       &server.request_cursor_shape, handle_request_cursor_shape);
}
