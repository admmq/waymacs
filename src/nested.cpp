/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* nested.cpp - in a window of another Wayland desktop (test/run-nested.sh),
 * ask the desktop to pass its own shortcuts on while that window has the
 * keyboard (keyboard-shortcuts-inhibit), so that Super+1, Alt+Tab and the
 * like reach Emacs.  GNOME asks the user first, and Super+Escape gives them
 * back to it. */
#include "waymacs.h"
#include <wayland-client.h>
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
extern "C" {
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
}

/* the desktop's objects, while the backend's connection to it lasts */
static wl_display *host;
static zwp_keyboard_shortcuts_inhibit_manager_v1 *inhibit_manager;
static std::vector<wl_seat *> host_seats;
static Listener new_host_window, host_gone;

static const zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
	[](void *, zwp_keyboard_shortcuts_inhibitor_v1 *) {
		wlr_log(WLR_INFO, "the desktop passes its shortcuts on while our window has the keyboard");
	},
	[](void *, zwp_keyboard_shortcuts_inhibitor_v1 *) {
		wlr_log(WLR_INFO, "the desktop handles its shortcuts itself again");
	},
};

/* An output of the nested backend: a window of the desktop's */
struct HostWindow {
	explicit HostWindow(struct wlr_output *output);
	~HostWindow();
	std::vector<zwp_keyboard_shortcuts_inhibitor_v1 *> inhibitors;	/* one per seat */
	Listener frame, destroy;
};

HostWindow::HostWindow(struct wlr_output *output)
{
	/* asked once the window is on the screen (its first frame): GNOME's
	 * question then comes with it */
	frame.listen(&output->events.frame, [this, output](void *) {
		frame.stop();
		if (!inhibit_manager)
			return;
		wl_surface *surface = wlr_wl_output_get_surface(output);
		for (wl_seat *seat : host_seats) {
			auto *inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
				inhibit_manager, surface, seat);
			zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor, &inhibitor_listener,
									 nullptr);
			inhibitors.push_back(inhibitor);
		}
		wl_display_flush(host);
	});
	destroy.listen(&output->events.destroy, [this](void *) { delete this; });
}

HostWindow::~HostWindow()
{
	for (zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor : inhibitors)
		zwp_keyboard_shortcuts_inhibitor_v1_destroy(inhibitor);
}

static void handle_global(void *data, wl_registry *registry, uint32_t name,
			  const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_seat_interface.name))
		host_seats.push_back(static_cast<wl_seat *>(
			wl_registry_bind(registry, name, &wl_seat_interface, 1)));
	else if (!strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
		inhibit_manager = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1 *>(
			wl_registry_bind(registry, name,
					 &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
}

static const wl_registry_listener registry_listener = {
	handle_global,
	[](void *, wl_registry *, uint32_t) {},
};

/* drop our objects of the desktop's: the connection to it is closing */
static void forget_host()
{
	new_host_window.stop();
	host_gone.stop();
	if (inhibit_manager)
		zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(inhibit_manager);
	inhibit_manager = nullptr;
	for (wl_seat *seat : host_seats)
		wl_seat_destroy(seat);
	host_seats.clear();
	host = nullptr;
}

/* the backend showing us in windows of a Wayland desktop, if any */
static struct wlr_backend *nested_backend()
{
	struct wlr_backend *found = nullptr;
	auto check = [](struct wlr_backend *backend, void *data) {
		if (wlr_backend_is_wl(backend))
			*static_cast<struct wlr_backend **>(data) = backend;
	};
	if (wlr_backend_is_multi(server.backend))
		wlr_multi_for_each_backend(server.backend, check, &found);
	else
		check(server.backend, &found);
	return found;
}

/* Before the backend starts: it opens its windows then. */
void nested_init()
{
	struct wlr_backend *backend = nested_backend();
	if (!backend)
		return;
	host = wlr_wl_backend_get_remote_display(backend);

	/* the desktop's globals, on an event queue of our own: the backend's
	 * pending events wait for it to dispatch them */
	wl_event_queue *queue = wl_display_create_queue(host);
	auto *host_on_queue = static_cast<wl_display *>(wl_proxy_create_wrapper(host));
	wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(host_on_queue), queue);
	wl_registry *registry = wl_display_get_registry(host_on_queue);
	wl_proxy_wrapper_destroy(host_on_queue);
	wl_registry_add_listener(registry, &registry_listener, nullptr);
	wl_display_roundtrip_queue(host, queue);
	wl_registry_destroy(registry);
	/* what they send from now on is the backend's to dispatch */
	for (wl_seat *seat : host_seats)
		wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(seat), nullptr);
	if (inhibit_manager)
		wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(inhibit_manager), nullptr);
	wl_event_queue_destroy(queue);

	if (!inhibit_manager) {
		wlr_log(WLR_INFO, "the desktop cannot pass its shortcuts (Super+1...) on to waymacs");
		forget_host();
		return;
	}
	new_host_window.listen(&backend->events.new_output, [](void *data) {
		new HostWindow(static_cast<struct wlr_output *>(data));
	});
	host_gone.listen(&backend->events.destroy, [](void *) { forget_host(); });
}
