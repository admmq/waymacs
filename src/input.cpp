/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* input.cpp - keyboards (prefix keys, simulation keys), the pointer,
 * interactive move/resize, seat requests. */
#include "waymacs.h"
#include <linux/input-event-codes.h>

static uint32_t now_msec()
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* ---- keyboards ---- */

struct Keyboard {
	Keyboard(struct wlr_keyboard *keyboard, bool is_virtual);
	~Keyboard() { erase(server.keyboards, this); }
	void handle_key(wlr_keyboard_key_event *event);
	void swallow_release(uint32_t keycode) { swallowed_keycodes.push_back(keycode); }
	bool release_is_swallowed(uint32_t keycode);

	struct wlr_keyboard *wlr_keyboard;
	bool is_virtual;
	std::vector<uint32_t> swallowed_keycodes;	/* presses eaten; eat their releases */
	uint32_t held_physical_keycode = 0;		/* a simulation key held down... */
	uint32_t held_injected_keycode = 0;		/* ...and the key we pressed for it */
	Listener modifiers, key, destroy;
};

static const char *or_null(const std::string &text)
{
	return text.empty() ? nullptr : text.c_str();
}

static void set_keymap(wlr_keyboard *keyboard)
{
	xkb_rule_names names = {};
	names.layout = or_null(server.xkb_layout);
	names.variant = or_null(server.xkb_variant);
	names.options = or_null(server.xkb_options);
	xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap)
		wlr_keyboard_set_keymap(keyboard, keymap);
	else
		wlr_log(WLR_ERROR, "bad xkb settings: layout `%s' variant `%s' options `%s'",
			server.xkb_layout.c_str(), server.xkb_variant.c_str(),
			server.xkb_options.c_str());
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

void keyboards_reconfigure()
{
	for (Keyboard *keyboard : server.keyboards) {
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
static std::vector<KeySpec> key_candidates(wlr_keyboard *keyboard, xkb_keycode_t keycode)
{
	xkb_state *state = keyboard->xkb_state;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard) & MODIFIER_MASK;
	uint32_t consumed = 0;
	for (int i = 0; i < WLR_MODIFIER_COUNT; i++) {
		xkb_mod_index_t index = keyboard->mod_indexes[i];
		if (index != XKB_MOD_INVALID &&
		    xkb_state_mod_index_is_consumed2(state, keycode, index, XKB_CONSUMED_MODE_XKB) == 1)
			consumed |= 1u << i;
	}
	std::vector<KeySpec> candidates;
	auto add = [&candidates](const xkb_keysym_t *keysyms, int count, uint32_t modifiers) {
		for (int i = 0; i < count; i++)
			candidates.push_back({ modifiers, xkb_keysym_to_lower(keysyms[i]) });
	};
	const xkb_keysym_t *keysyms;
	int count = xkb_state_key_get_syms(state, keycode, &keysyms);
	add(keysyms, count, modifiers & ~consumed);
	/* level one of the active layout, then of the first layout */
	xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
	for (xkb_layout_index_t current = layout;; current = 0) {
		count = xkb_keymap_key_get_syms_by_level(keyboard->keymap, keycode, current, 0, &keysyms);
		add(keysyms, count, modifiers);
		if (current == 0)
			break;
	}
	return candidates;
}

static bool key_matches(const KeySpec &key, const std::vector<KeySpec> &candidates)
{
	return std::any_of(candidates.begin(), candidates.end(), [&key](const KeySpec &candidate) {
		return key.keysym == candidate.keysym && key.modifiers == candidate.modifiers;
	});
}

static bool any_key_matches(const std::vector<KeySpec> &keys,
			    const std::vector<KeySpec> &candidates)
{
	return std::any_of(keys.begin(), keys.end(), [&candidates](const KeySpec &key) {
		return key_matches(key, candidates);
	});
}

/* The keycode typing KEYSYM, and whether Shift is needed for it. */
static bool find_keycode(wlr_keyboard *keyboard, xkb_keysym_t keysym,
			 xkb_keycode_t &keycode, uint32_t &shift)
{
	xkb_keymap *keymap = keyboard->keymap;
	xkb_layout_index_t layout = xkb_state_serialize_layout(keyboard->xkb_state,
							       XKB_STATE_LAYOUT_EFFECTIVE);
	for (xkb_level_index_t level = 0; level < 2; level++) {
		for (xkb_keycode_t code = xkb_keymap_min_keycode(keymap);
		     code <= xkb_keymap_max_keycode(keymap); code++) {
			const xkb_keysym_t *keysyms;
			int count = xkb_keymap_key_get_syms_by_level(keymap, code, layout, level, &keysyms);
			if (std::count(keysyms, keysyms + count, keysym)) {
				keycode = code;
				shift = level ? WLR_MODIFIER_SHIFT : 0;
				return true;
			}
		}
	}
	return false;
}

static bool can_type(wlr_keyboard *keyboard, const std::vector<KeySpec> &keys)
{
	xkb_keycode_t keycode;
	uint32_t shift;
	return keyboard && keyboard->xkb_state &&
	       std::all_of(keys.begin(), keys.end(), [&](const KeySpec &key) {
		return find_keycode(keyboard, key.keysym, keycode, shift);
	});
}

static xkb_mod_mask_t xkb_modifier_mask(wlr_keyboard *keyboard, uint32_t modifiers)
{
	xkb_mod_mask_t mask = 0;
	for (int i = 0; i < WLR_MODIFIER_COUNT; i++)
		if ((modifiers & (1u << i)) && keyboard->mod_indexes[i] != XKB_MOD_INVALID)
			mask |= 1u << keyboard->mod_indexes[i];
	return mask;
}

/* Type KEYS in the focused client.  With HOLD_LAST the last key stays
 * down (so the client's own key repeat works); its keycode is returned. */
static uint32_t send_keys(const std::vector<KeySpec> &keys, bool hold_last)
{
	/* virtual keyboards (wtype...) have minimal keymaps: type with our
	 * own keyboard when the active one cannot */
	wlr_keyboard *previous = wlr_seat_get_keyboard(server.seat);
	wlr_keyboard *keyboard = previous;
	if (!can_type(keyboard, keys)) {
		keyboard = &server.inject_keyboard;
		wlr_seat_set_keyboard(server.seat, keyboard);
	}
	if (!keyboard->xkb_state)
		return 0;
	wlr_keyboard_modifiers saved_modifiers = (previous ? previous : keyboard)->modifiers;
	xkb_layout_index_t layout = xkb_state_serialize_layout(keyboard->xkb_state,
							       XKB_STATE_LAYOUT_EFFECTIVE);
	uint32_t time_msec = now_msec(), held_keycode = 0;
	for (size_t i = 0; i < keys.size(); i++) {
		xkb_keycode_t keycode;
		uint32_t shift;
		if (!find_keycode(keyboard, keys[i].keysym, keycode, shift)) {
			char name[64];
			xkb_keysym_get_name(keys[i].keysym, name, sizeof name);
			wlr_log(WLR_INFO, "no key in the keymap types %s", name);
			continue;
		}
		wlr_keyboard_modifiers modifiers = {};
		modifiers.depressed = xkb_modifier_mask(keyboard, keys[i].modifiers | shift);
		modifiers.group = layout;
		wlr_seat_keyboard_notify_modifiers(server.seat, &modifiers);
		/* the seat speaks evdev keycodes, xkb adds 8 */
		wlr_seat_keyboard_notify_key(server.seat, time_msec, keycode - 8,
					     WL_KEYBOARD_KEY_STATE_PRESSED);
		if (hold_last && i == keys.size() - 1) {
			held_keycode = keycode - 8;
			break;
		}
		wlr_seat_keyboard_notify_key(server.seat, time_msec, keycode - 8,
					     WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	if (!held_keycode) {
		if (previous && previous != keyboard)
			wlr_seat_set_keyboard(server.seat, previous);
		wlr_seat_keyboard_notify_modifiers(server.seat, &saved_modifiers);
	}
	return held_keycode;
}

void inject_keys(const std::vector<KeySpec> &keys)
{
	send_keys(keys, false);
}

bool Keyboard::release_is_swallowed(uint32_t keycode)
{
	auto found = std::find(swallowed_keycodes.begin(), swallowed_keycodes.end(), keycode);
	if (found == swallowed_keycodes.end())
		return false;
	swallowed_keycodes.erase(found);
	return true;
}

/* Keys the compositor always handles: VT switching, emergency exit. */
static bool handle_compositor_key(const std::vector<KeySpec> &candidates)
{
	const uint32_t emergency_exit = WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT;
	for (const KeySpec &candidate : candidates) {
		xkb_keysym_t keysym = candidate.keysym;
		if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
			if (server.session)
				wlr_session_change_vt(server.session, keysym - XKB_KEY_XF86Switch_VT_1 + 1);
			return true;
		}
		/* not while locked: anyone could end the session */
		if (keysym == XKB_KEY_BackSpace && candidate.modifiers == emergency_exit &&
		    !server.locked) {
			wlr_log(WLR_INFO, "Ctrl+Alt+Shift+BackSpace: quitting");
			wl_display_terminate(server.display);
			return true;
		}
	}
	return false;
}

/* The application holding the keyboard, if any. */
static View *focused_app()
{
	View *view = server.focused_view;
	if (server.focused_layer || !view || view->is_frame || !view->mapped ||
	    server.seat->keyboard_state.focused_surface != view->surface())
		return nullptr;
	return view;
}

void Keyboard::handle_key(wlr_keyboard_key_event *event)
{
	wlr_seat_set_keyboard(server.seat, wlr_keyboard);
	if (event->time_msec == 0)	/* some virtual keyboards send no time */
		event->time_msec = now_msec();
	auto pass_on = [event](uint32_t keycode) {
		wlr_seat_keyboard_notify_key(server.seat, event->time_msec, keycode, event->state);
	};

	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (held_physical_keycode && held_physical_keycode == event->keycode) {
			pass_on(held_injected_keycode);
			wlr_seat_keyboard_notify_modifiers(server.seat, &wlr_keyboard->modifiers);
			held_physical_keycode = 0;
		} else if (!release_is_swallowed(event->keycode)) {
			pass_on(event->keycode);
		}
		return;
	}
	if (!wlr_keyboard->xkb_state) {
		pass_on(event->keycode);
		return;
	}
	std::vector<KeySpec> candidates = key_candidates(wlr_keyboard, event->keycode + 8);
	if (handle_compositor_key(candidates)) {
		swallow_release(event->keycode);
		return;
	}
	View *app = server.locked ? nullptr : focused_app();	/* locked: all to the locker */
	bool line_mode = app && !app->char_mode && !app->fullscreen;
	if (app && (any_key_matches(server.global_keys, candidates) ||
		    (line_mode && any_key_matches(server.prefix_keys, candidates)))) {
		/* Give the keyboard to the Emacs frame and replay the press
		 * there; Emacs hands it back when it is done. */
		View *frame = frame_is_visible(app->frame) ? app->frame : nullptr;
		if (!frame && focused_output())
			frame = focused_output()->active_frame;
		if (frame) {
			focus_frame_for_key(frame, event->keycode);
			pass_on(event->keycode);
			return;
		}
	}
	for (const SimulationKey &simulation : server.simulation_keys) {
		if (!line_mode || !key_matches(simulation.from, candidates))
			continue;
		uint32_t held = send_keys(simulation.to, true);
		if (held && !held_physical_keycode) {
			held_physical_keycode = event->keycode;
			held_injected_keycode = held;
		} else {
			swallow_release(event->keycode);
		}
		return;
	}
	pass_on(event->keycode);
}

Keyboard::Keyboard(struct wlr_keyboard *keyboard, bool is_virtual)
	: wlr_keyboard(keyboard), is_virtual(is_virtual)
{
	if (!is_virtual)	/* virtual keyboards bring their own keymap */
		set_keymap(keyboard);
	wlr_keyboard_set_repeat_info(keyboard, server.repeat_rate, server.repeat_delay);
	modifiers.listen(&keyboard->events.modifiers, [this](void *) {
		wlr_seat_set_keyboard(server.seat, wlr_keyboard);
		wlr_seat_keyboard_notify_modifiers(server.seat, &wlr_keyboard->modifiers);
	});
	key.listen(&keyboard->events.key, [this](void *data) {
		handle_key(static_cast<wlr_keyboard_key_event *>(data));
	});
	destroy.listen(&keyboard->base.events.destroy, [this](void *) { delete this; });
	server.keyboards.push_back(this);
	/* A virtual keyboard has no keymap until its client sends one; made
	 * the seat's keyboard now, clients would be told "no keymap", which
	 * some (swaylock) treat as fatal.  Its first key event selects it. */
	if (keyboard->keymap)
		wlr_seat_set_keyboard(server.seat, keyboard);
}

/* ---- pointer ---- */

static CursorMode cursor_mode = CursorMode::passthrough;
static View *grabbed_view;
static double grab_cursor_x, grab_cursor_y;
static wlr_box grab_box;
static uint32_t grab_edges;

/* The surface at layout coordinates X, Y and the view or layer it belongs to. */
static wlr_surface *surface_at(double x, double y, double &surface_x, double &surface_y,
			       View *&view, Layer *&layer)
{
	view = nullptr;
	layer = nullptr;
	wlr_scene_node *node = wlr_scene_node_at(&server.scene->tree.node, x, y,
						 &surface_x, &surface_y);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER)
		return nullptr;
	wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	if (!scene_surface)
		return nullptr;
	for (wlr_scene_tree *tree = node->parent; tree; tree = tree->node.parent) {
		if (!tree->node.data)
			continue;
		auto *owner = static_cast<SceneOwner *>(tree->node.data);
		if (owner->kind == SceneOwner::VIEW)
			view = static_cast<View *>(owner);
		else
			layer = static_cast<Layer *>(owner);
		break;
	}
	return scene_surface->surface;
}

void begin_grab(View *view, CursorMode mode, uint32_t edges)
{
	if (cursor_mode != CursorMode::passthrough || !view->floating || view->fullscreen)
		return;
	cursor_mode = mode;
	grabbed_view = view;
	grab_edges = edges ? edges : WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT;
	grab_box = view->floating_box;
	grab_cursor_x = server.cursor->x;
	grab_cursor_y = server.cursor->y;
}

void end_grab(View *view)
{
	if (view && view != grabbed_view)
		return;
	cursor_mode = CursorMode::passthrough;
	grabbed_view = nullptr;
}

static void grab_motion()
{
	int delta_x = (int)(server.cursor->x - grab_cursor_x);
	int delta_y = (int)(server.cursor->y - grab_cursor_y);
	wlr_box box = grab_box;
	if (cursor_mode == CursorMode::move) {
		box.x += delta_x;
		box.y += delta_y;
	} else {
		if (grab_edges & WLR_EDGE_LEFT) {
			box.x += delta_x;
			box.width -= delta_x;
		} else if (grab_edges & WLR_EDGE_RIGHT) {
			box.width += delta_x;
		}
		if (grab_edges & WLR_EDGE_TOP) {
			box.y += delta_y;
			box.height -= delta_y;
		} else if (grab_edges & WLR_EDGE_BOTTOM) {
			box.height += delta_y;
		}
		if (box.width < 64 || box.height < 64)
			return;
	}
	grabbed_view->floating_box = box;
	arrange();
}

static void process_motion(uint32_t time_msec)
{
	if (cursor_mode != CursorMode::passthrough && grabbed_view) {
		grab_motion();
		return;
	}
	double surface_x, surface_y;
	View *view;
	Layer *layer;
	wlr_surface *surface = surface_at(server.cursor->x, server.cursor->y,
					  surface_x, surface_y, view, layer);
	if (server.locked && !is_lock_surface(surface))
		surface = nullptr;
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
void update_pointer_focus()
{
	if (cursor_mode == CursorMode::passthrough)
		process_motion(0);
}

/* Click to focus.  Emacs learns about it with an `activate' event. */
static void focus_clicked(View *view, Layer *layer)
{
	if (layer) {
		if (!layer->keyboard_interactivity_is(ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)) {
			server.focused_layer = layer;
			apply_keyboard_focus();
		}
		return;
	}
	if (server.focused_layer && !server.focused_layer->keyboard_interactivity_is(
		    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE))
		server.focused_layer = nullptr;
	if (view && view != server.focused_view) {
		focus_view(view, false);
		ipc_send("(activate :id %u)", view->id);
	} else {
		apply_keyboard_focus();
	}
}

static void handle_cursor_button(void *data)
{
	auto *event = static_cast<wlr_pointer_button_event *>(data);
	if (server.locked) {	/* only to the locker, focuses nothing */
		wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button,
					       event->state);
		return;
	}
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		double surface_x, surface_y;
		View *view;
		Layer *layer;
		surface_at(server.cursor->x, server.cursor->y, surface_x, surface_y, view, layer);
		wlr_keyboard *keyboard = wlr_seat_get_keyboard(server.seat);
		uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		focus_clicked(view, layer);
		/* Super + left/right drag moves/resizes floating windows */
		if (view && !view->is_frame && view->floating && !view->fullscreen &&
		    (modifiers & WLR_MODIFIER_LOGO) &&
		    (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
			begin_grab(view, event->button == BTN_LEFT ? CursorMode::move : CursorMode::resize, 0);
			return;
		}
	} else {
		end_grab();
	}
	wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button, event->state);
}

/* libinput leaves tap-to-click off; most people want it on touchpads */
static void configure_pointer(wlr_input_device *device)
{
	if (!wlr_input_device_is_libinput(device))
		return;
	libinput_device *libinput_device = wlr_libinput_get_device_handle(device);
	if (libinput_device_config_tap_get_finger_count(libinput_device) > 0)
		libinput_device_config_tap_set_enabled(libinput_device, LIBINPUT_CONFIG_TAP_ENABLED);
}

/* ---- the laptop lid: Emacs decides what to do about it ---- */

struct LidSwitch {
	explicit LidSwitch(wlr_input_device *device);
	Listener toggle, destroy;
};

LidSwitch::LidSwitch(wlr_input_device *device)
{
	toggle.listen(&wlr_switch_from_input_device(device)->events.toggle, [](void *data) {
		auto *event = static_cast<wlr_switch_toggle_event *>(data);
		if (event->switch_type != WLR_SWITCH_TYPE_LID)
			return;
		bool closed = event->switch_state == WLR_SWITCH_STATE_ON;
		if (closed == server.lid_closed)
			return;
		server.lid_closed = closed;
		wlr_log(WLR_INFO, "lid %s", closed ? "closed" : "opened");
		ipc_send("(lid :closed %s)", closed ? "t" : "nil");
	});
	destroy.listen(&device->events.destroy, [this](void *) { delete this; });
}

/* ---- devices and seat requests ---- */

static void handle_new_input(void *data)
{
	auto *device = static_cast<wlr_input_device *>(data);
	if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		new Keyboard(wlr_keyboard_from_input_device(device), false);
	} else if (device->type == WLR_INPUT_DEVICE_POINTER) {
		configure_pointer(device);
		wlr_cursor_attach_input_device(server.cursor, device);
	} else if (device->type == WLR_INPUT_DEVICE_SWITCH) {
		new LidSwitch(device);
	}
}

void input_init()
{
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	const char *cursor_size = getenv("XCURSOR_SIZE");
	server.xcursor_manager = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"),
		cursor_size && atoi(cursor_size) > 0 ? atoi(cursor_size) : 24);
	server.listen(&server.cursor->events.motion, [](void *data) {
		auto *event = static_cast<wlr_pointer_motion_event *>(data);
		wlr_cursor_move(server.cursor, &event->pointer->base, event->delta_x, event->delta_y);
		process_motion(event->time_msec);
	});
	server.listen(&server.cursor->events.motion_absolute, [](void *data) {
		auto *event = static_cast<wlr_pointer_motion_absolute_event *>(data);
		wlr_cursor_warp_absolute(server.cursor, &event->pointer->base, event->x, event->y);
		process_motion(event->time_msec);
	});
	server.listen(&server.cursor->events.button, handle_cursor_button);
	server.listen(&server.cursor->events.axis, [](void *data) {
		auto *event = static_cast<wlr_pointer_axis_event *>(data);
		wlr_seat_pointer_notify_axis(server.seat, event->time_msec, event->orientation,
					     event->delta, event->delta_discrete, event->source,
					     event->relative_direction);
	});
	server.listen(&server.cursor->events.frame, [](void *) {
		wlr_seat_pointer_notify_frame(server.seat);
	});

	static const wlr_keyboard_impl inject_keyboard_impl = { "waymacs-inject", nullptr };
	wlr_keyboard_init(&server.inject_keyboard, &inject_keyboard_impl, "waymacs-inject");
	set_keymap(&server.inject_keyboard);

	server.seat = wlr_seat_create(server.display, "seat0");
	wlr_seat_set_capabilities(server.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	server.listen(&server.seat->events.request_set_cursor, [](void *data) {
		auto *event = static_cast<wlr_seat_pointer_request_set_cursor_event *>(data);
		if (server.seat->pointer_state.focused_client == event->seat_client)
			wlr_cursor_set_surface(server.cursor, event->surface, event->hotspot_x,
					       event->hotspot_y);
	});
	server.listen(&server.seat->events.request_set_selection, [](void *data) {
		auto *event = static_cast<wlr_seat_request_set_selection_event *>(data);
		wlr_seat_set_selection(server.seat, event->source, event->serial);
	});
	server.listen(&server.seat->events.request_set_primary_selection, [](void *data) {
		auto *event = static_cast<wlr_seat_request_set_primary_selection_event *>(data);
		wlr_seat_set_primary_selection(server.seat, event->source, event->serial);
	});
	server.listen(&server.seat->events.request_start_drag, [](void *data) {
		auto *event = static_cast<wlr_seat_request_start_drag_event *>(data);
		if (wlr_seat_validate_pointer_grab_serial(server.seat, event->origin, event->serial))
			wlr_seat_start_pointer_drag(server.seat, event->drag, event->serial);
		else
			wlr_data_source_destroy(event->drag->source);
	});

	server.listen(&server.backend->events.new_input, handle_new_input);
	server.listen(&wlr_virtual_keyboard_manager_v1_create(server.display)
		      ->events.new_virtual_keyboard, [](void *data) {
		new Keyboard(&static_cast<wlr_virtual_keyboard_v1 *>(data)->keyboard, true);
	});
	server.listen(&wlr_virtual_pointer_manager_v1_create(server.display)
		      ->events.new_virtual_pointer, [](void *data) {
		auto *event = static_cast<wlr_virtual_pointer_v1_new_pointer_event *>(data);
		wlr_cursor_attach_input_device(server.cursor, &event->new_pointer->pointer.base);
	});
	server.listen(&wlr_cursor_shape_manager_v1_create(server.display, 1)
		      ->events.request_set_shape, [](void *data) {
		auto *event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event *>(data);
		if (server.seat->pointer_state.focused_client == event->seat_client)
			wlr_cursor_set_xcursor(server.cursor, server.xcursor_manager,
					       wlr_cursor_shape_v1_name(event->shape));
	});
}
