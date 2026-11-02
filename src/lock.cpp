/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* lock.cpp - screen lockers (ext-session-lock-v1: swaylock...).
 *
 * While the session is locked only the locker's surfaces are shown and
 * only they get input: every monitor is covered, keys are not
 * intercepted, Emacs cannot take the keyboard.  If the locker dies
 * without unlocking, the session stays locked (the cover turns red) until
 * a new locker unlocks it.
 */
#include "waymacs.h"

struct LockSurface {
	explicit LockSurface(wlr_session_lock_surface_v1 *surface);
	~LockSurface();
	wlr_session_lock_surface_v1 *lock_surface;
	wlr_scene_tree *scene_tree;
	int configured_width = 0, configured_height = 0;
	Listener map, destroy;
};

/* a live locker */
struct Lock {
	explicit Lock(wlr_session_lock_v1 *lock);
	wlr_session_lock_v1 *lock;
	Listener new_surface, unlock, destroy;
};

static Lock *current_lock;
static const float locked_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static const float abandoned_color[4] = { 0.6f, 0.0f, 0.0f, 1.0f };	/* locker died */

bool is_lock_surface(wlr_surface *surface)
{
	return surface && wlr_session_lock_surface_v1_try_from_wlr_surface(surface);
}

/* The locker's surface on the focused monitor, or else any of them. */
wlr_surface *lock_keyboard_target()
{
	if (!current_lock)
		return nullptr;
	Output *output = focused_output();
	wlr_session_lock_surface_v1 *lock_surface, *fallback = nullptr;
	wl_list_for_each(lock_surface, &current_lock->lock->surfaces, link) {
		if (!lock_surface->surface->mapped)
			continue;
		if (output && lock_surface->output == output->wlr_output)
			return lock_surface->surface;
		if (!fallback)
			fallback = lock_surface;
	}
	return fallback ? fallback->surface : nullptr;
}

/* Called by arrange(): covers and locker surfaces fill every monitor. */
void lock_arrange()
{
	wlr_scene_node_set_enabled(&server.lock_tree->node, server.locked);
	for (Output *output : server.outputs) {
		wlr_scene_node_set_position(&output->lock_background->node, output->box.x, output->box.y);
		wlr_scene_rect_set_size(output->lock_background, output->box.width, output->box.height);
		wlr_scene_rect_set_color(output->lock_background,
					 current_lock ? locked_color : abandoned_color);
	}
	if (!current_lock)
		return;
	wlr_session_lock_surface_v1 *lock_surface;
	wl_list_for_each(lock_surface, &current_lock->lock->surfaces, link) {
		auto *surface = static_cast<LockSurface *>(lock_surface->data);
		auto *output = lock_surface->output ? static_cast<Output *>(lock_surface->output->data) : nullptr;
		if (!surface || !output)
			continue;
		const wlr_box &box = output->box;
		wlr_scene_node_set_position(&surface->scene_tree->node, box.x, box.y);
		if (box.width != surface->configured_width || box.height != surface->configured_height) {
			surface->configured_width = box.width;
			surface->configured_height = box.height;
			wlr_session_lock_surface_v1_configure(lock_surface, box.width, box.height);
		}
	}
}

LockSurface::LockSurface(wlr_session_lock_surface_v1 *surface) : lock_surface(surface)
{
	scene_tree = wlr_scene_subsurface_tree_create(server.lock_tree, surface->surface);
	surface->data = this;
	map.listen(&surface->surface->events.map, [](void *) {
		apply_keyboard_focus();
		update_pointer_focus();
	});
	destroy.listen(&surface->events.destroy, [this](void *) { delete this; });
	lock_arrange();		/* sends its first configure */
}

LockSurface::~LockSurface()
{
	lock_surface->data = nullptr;
	apply_keyboard_focus();
}

Lock::Lock(wlr_session_lock_v1 *session_lock) : lock(session_lock)
{
	new_surface.listen(&lock->events.new_surface, [](void *data) {
		new LockSurface(static_cast<wlr_session_lock_surface_v1 *>(data));
	});
	unlock.listen(&lock->events.unlock, [](void *) {
		wlr_log(WLR_INFO, "session unlocked");
		server.locked = false;
		arrange();
		apply_keyboard_focus();
	});
	destroy.listen(&lock->events.destroy, [this](void *) {
		delete this;
		current_lock = nullptr;
		if (server.locked)
			wlr_log(WLR_ERROR, "the screen locker died: the session stays locked, "
				"start a new locker to unlock it");
		arrange();
		apply_keyboard_focus();
	});
}

static void handle_new_lock(void *data)
{
	auto *lock = static_cast<wlr_session_lock_v1 *>(data);
	if (current_lock) {
		/* already locked by a live locker */
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	wlr_log(WLR_INFO, "session locked");
	current_lock = new Lock(lock);
	server.locked = true;
	/* no menus or interactive moves survive locking */
	wlr_seat_pointer_end_grab(server.seat);
	wlr_seat_keyboard_end_grab(server.seat);
	end_grab();
	arrange();		/* covers every monitor */
	apply_keyboard_focus();
	wlr_session_lock_v1_send_locked(lock);
}

void lock_init()
{
	server.lock_tree = wlr_scene_tree_create(&server.scene->tree);	/* topmost */
	wlr_scene_node_set_enabled(&server.lock_tree->node, false);
	server.listen(&wlr_session_lock_manager_v1_create(server.display)->events.new_lock,
		      handle_new_lock);
}
