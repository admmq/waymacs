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

struct lock_surface {
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_scene_tree *scene_tree;
	int configured_width, configured_height;
	struct wl_listener map, destroy;
};

static const float locked_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
static const float abandoned_color[4] = { 0.6f, 0.0f, 0.0f, 1.0f };	/* locker died */

bool is_lock_surface(struct wlr_surface *surface)
{
	return surface && wlr_session_lock_surface_v1_try_from_wlr_surface(surface);
}

/* The locker's surface on the focused monitor, or else any of them. */
struct wlr_surface *lock_keyboard_target(void)
{
	if (!server.lock)
		return NULL;
	struct output *output = focused_output();
	struct wlr_session_lock_surface_v1 *lock_surface, *fallback = NULL;
	wl_list_for_each(lock_surface, &server.lock->surfaces, link) {
		if (!lock_surface->surface->mapped)
			continue;
		if (output && lock_surface->output == output->wlr_output)
			return lock_surface->surface;
		if (!fallback)
			fallback = lock_surface;
	}
	return fallback ? fallback->surface : NULL;
}

/* Called by arrange(): covers and locker surfaces fill every monitor. */
void lock_arrange(void)
{
	wlr_scene_node_set_enabled(&server.lock_tree->node, server.locked);
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		wlr_scene_node_set_position(&output->lock_background->node,
					    output->box.x, output->box.y);
		wlr_scene_rect_set_size(output->lock_background, output->box.width,
					output->box.height);
		wlr_scene_rect_set_color(output->lock_background,
					 server.lock ? locked_color : abandoned_color);
	}
	if (!server.lock)
		return;
	struct wlr_session_lock_surface_v1 *lock_surface;
	wl_list_for_each(lock_surface, &server.lock->surfaces, link) {
		struct lock_surface *surface = (struct lock_surface *)lock_surface->data;
		struct output *surface_output = lock_surface->output ?
			(struct output *)lock_surface->output->data : NULL;
		if (!surface || !surface_output)
			continue;
		struct wlr_box *box = &surface_output->box;
		wlr_scene_node_set_position(&surface->scene_tree->node, box->x, box->y);
		if (box->width != surface->configured_width ||
		    box->height != surface->configured_height) {
			surface->configured_width = box->width;
			surface->configured_height = box->height;
			wlr_session_lock_surface_v1_configure(lock_surface, box->width,
							      box->height);
		}
	}
}

static void handle_lock_surface_destroy(struct wl_listener *listener, void *data)
{
	struct lock_surface *surface = wl_container_of(listener, surface, destroy);
	stop_listening(&surface->map, &surface->destroy);
	surface->lock_surface->data = NULL;
	free(surface);
	apply_keyboard_focus(UINT32_MAX);
}

static void handle_lock_new_surface(struct wl_listener *listener, void *data)
{
	auto *lock_surface = (struct wlr_session_lock_surface_v1 *)data;
	struct lock_surface *surface = (struct lock_surface *)calloc(1, sizeof *surface);
	surface->lock_surface = lock_surface;
	surface->scene_tree = wlr_scene_subsurface_tree_create(server.lock_tree,
							       lock_surface->surface);
	lock_surface->data = surface;
	LISTEN(&lock_surface->surface->events.map, &surface->map,
	       [](struct wl_listener *listener, void *data) {
		apply_keyboard_focus(UINT32_MAX);
		update_pointer_focus();
	});
	LISTEN(&lock_surface->events.destroy, &surface->destroy, handle_lock_surface_destroy);
	lock_arrange();		/* sends its first configure */
}

static void handle_unlock(struct wl_listener *listener, void *data)
{
	wlr_log(WLR_INFO, "session unlocked");
	server.locked = false;
	arrange();
	apply_keyboard_focus(UINT32_MAX);
}

static void handle_lock_destroy(struct wl_listener *listener, void *data)
{
	stop_listening(&server.lock_new_surface, &server.lock_unlock, &server.lock_destroy);
	server.lock = NULL;
	if (server.locked)
		wlr_log(WLR_ERROR, "the screen locker died: the session stays locked, "
			"start a new locker to unlock it");
	arrange();
	apply_keyboard_focus(UINT32_MAX);
}

static void handle_new_lock(struct wl_listener *listener, void *data)
{
	auto *lock = (struct wlr_session_lock_v1 *)data;
	if (server.lock) {
		/* already locked by a live locker */
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	wlr_log(WLR_INFO, "session locked");
	server.lock = lock;
	server.locked = true;
	LISTEN(&lock->events.new_surface, &server.lock_new_surface, handle_lock_new_surface);
	LISTEN(&lock->events.unlock, &server.lock_unlock, handle_unlock);
	LISTEN(&lock->events.destroy, &server.lock_destroy, handle_lock_destroy);
	/* no menus or interactive moves survive locking */
	wlr_seat_pointer_end_grab(server.seat);
	wlr_seat_keyboard_end_grab(server.seat);
	server.cursor_mode = CURSOR_PASSTHROUGH;
	server.grabbed_view = NULL;
	arrange();		/* covers every monitor */
	apply_keyboard_focus(UINT32_MAX);
	wlr_session_lock_v1_send_locked(lock);
}

void lock_init(void)
{
	server.lock_tree = wlr_scene_tree_create(&server.scene->tree);	/* topmost */
	wlr_scene_node_set_enabled(&server.lock_tree->node, false);
	struct wlr_session_lock_manager_v1 *manager =
		wlr_session_lock_manager_v1_create(server.display);
	LISTEN(&manager->events.new_lock, &server.new_lock, handle_new_lock);
	/* wlroots insists that nobody listens to the manager once it is gone */
	LISTEN(&manager->events.destroy, &server.lock_manager_destroy,
	       [](struct wl_listener *listener, void *data) {
		stop_listening(&server.new_lock, &server.lock_manager_destroy);
	});
}
