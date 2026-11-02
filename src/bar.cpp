/* Copyright (C) 2026 Adam Quandour
 * SPDX-License-Identifier: GPL-3.0-or-later (see COPYING) */
/* bar.cpp - what status bars such as waybar need.
 *
 * - river-status and river-control (from the River compositor): waymacs's
 *   numbered workspaces are River "tags", workspace N being bit N-1.
 *   waybar's river/tags module shows them and switches with a click,
 *   river/window shows the focused window's title.
 * - wlr-foreign-toplevel-management: the application windows, for
 *   taskbars (waybar's wlr/taskbar).
 *
 * Workspace numbers live in Emacs; it tells us each frame's number.
 * Clicks become events for Emacs, which does the switching.
 */
#include "waymacs.h"
#include "river-status-unstable-v1-protocol.h"
#include "river-control-unstable-v1-protocol.h"

struct OutputStatus {
	~OutputStatus();
	wl_resource *resource;
	struct wlr_output *wlr_output;
	bool sent = false;
	uint32_t sent_focused_tags = 0;
	std::vector<uint32_t> sent_view_tags;
};

struct SeatStatus {
	~SeatStatus();
	wl_resource *resource;
	bool sent = false;
	struct wlr_output *sent_output = nullptr;
	std::string sent_title;
};

/* a river-control client's command, word by word */
struct Control {
	std::vector<std::string> arguments;
};

static std::vector<OutputStatus *> output_statuses;
static std::vector<SeatStatus *> seat_statuses;

/* ---- river-status ---- */

static uint32_t workspace_tag(View *frame)
{
	if (!frame || frame->workspace < 1 || frame->workspace > 32)
		return 0;
	return 1u << (frame->workspace - 1);
}

/* the workspace of every window on OUTPUT */
static std::vector<uint32_t> view_tags(Output *output)
{
	std::vector<uint32_t> tags;
	if (!output)
		return tags;
	/* workspaces not shown yet belong to no output: list them on the
	 * focused one, where they will appear */
	if (output == focused_output())
		for (int bit = 0; bit < 32; bit++)
			if (server.unshown_workspace_tags & (1u << bit))
				tags.push_back(1u << bit);
	for (View *view : server.views) {
		View *frame = frame_of(view);
		uint32_t tag = workspace_tag(frame);
		if (view->mapped && frame && frame->output == output && tag)
			tags.push_back(tag);
	}
	return tags;
}

static void send_output_status(OutputStatus *status)
{
	auto *output = status->wlr_output ? static_cast<Output *>(status->wlr_output->data) : nullptr;
	uint32_t focused_tags = output ? workspace_tag(output->active_frame) : 0;
	std::vector<uint32_t> tags = view_tags(output);
	if (!status->sent || focused_tags != status->sent_focused_tags)
		zriver_output_status_v1_send_focused_tags(status->resource, focused_tags);
	if (!status->sent || tags != status->sent_view_tags) {
		size_t size = tags.size() * sizeof tags[0];
		wl_array array = { size, size, tags.data() };
		zriver_output_status_v1_send_view_tags(status->resource, &array);
	}
	if (!status->sent && wl_resource_get_version(status->resource) >=
	    ZRIVER_OUTPUT_STATUS_V1_URGENT_TAGS_SINCE_VERSION)
		zriver_output_status_v1_send_urgent_tags(status->resource, 0);
	status->sent = true;
	status->sent_focused_tags = focused_tags;
	status->sent_view_tags = std::move(tags);
}

static wl_resource *client_output_resource(struct wlr_output *wlr_output, wl_client *client)
{
	wl_resource *resource;
	wl_resource_for_each(resource, &wlr_output->resources)
		if (wl_resource_get_client(resource) == client)
			return resource;
	return nullptr;
}

static void send_seat_status(SeatStatus *status)
{
	wl_client *client = wl_resource_get_client(status->resource);
	Output *output = focused_output();
	auto *wlr_output = output ? output->wlr_output : nullptr;
	if (!status->sent || wlr_output != status->sent_output) {
		wl_resource *resource;
		if (status->sent_output &&
		    (resource = client_output_resource(status->sent_output, client)))
			zriver_seat_status_v1_send_unfocused_output(status->resource, resource);
		if (wlr_output && (resource = client_output_resource(wlr_output, client)))
			zriver_seat_status_v1_send_focused_output(status->resource, resource);
		status->sent_output = wlr_output;
	}
	View *view = server.focused_view;
	const char *title = view && view->mapped ? view->title() : "";
	if (!strncmp(title, "waymacs-frame-", 14))	/* a frame being paired */
		title = "";
	if (!status->sent || status->sent_title != title) {
		zriver_seat_status_v1_send_focused_view(status->resource, title);
		status->sent_title = title;
	}
	if (!status->sent && wl_resource_get_version(status->resource) >=
	    ZRIVER_SEAT_STATUS_V1_MODE_SINCE_VERSION)
		zriver_seat_status_v1_send_mode(status->resource, "normal");
	status->sent = true;
}

static int announced_bar = -1;	/* what Emacs was told: 0 or 1, -1 for nothing */

/* Emacs hides its own workspace list while a bar shows them. */
static void announce_bar()
{
	int present = !output_statuses.empty();
	if (present != announced_bar && ipc_connected()) {
		ipc_send("(bar :workspaces %s)", present ? "t" : "nil");
		announced_bar = present;
	}
}

static void handle_destroy_request(wl_client *client, wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct zriver_output_status_v1_interface output_status_implementation = {
	handle_destroy_request,
};

static const struct zriver_seat_status_v1_interface seat_status_implementation = {
	handle_destroy_request,
};

/* a resource whose user data is a T, deleted with it */
template <typename T>
static wl_resource *create_resource(wl_client *client, const wl_interface *interface,
				    int version, uint32_t id, const void *implementation, T *data)
{
	wl_resource *resource = wl_resource_create(client, interface, version, id);
	if (!resource) {
		delete data;
		wl_client_post_no_memory(client);
		return nullptr;
	}
	wl_resource_set_implementation(resource, implementation, data, [](wl_resource *resource) {
		delete static_cast<T *>(wl_resource_get_user_data(resource));
	});
	return resource;
}


OutputStatus::~OutputStatus()
{
	erase(output_statuses, this);
	announce_bar();
}

SeatStatus::~SeatStatus()
{
	erase(seat_statuses, this);
}

static void get_output_status(wl_client *client, wl_resource *manager, uint32_t id,
			      wl_resource *output_resource)
{
	auto *status = new OutputStatus{};
	status->wlr_output = wlr_output_from_resource(output_resource);
	status->resource = create_resource(client, &zriver_output_status_v1_interface,
					   wl_resource_get_version(manager), id,
					   &output_status_implementation, status);
	if (!status->resource)
		return;
	output_statuses.push_back(status);
	send_output_status(status);
	announce_bar();
}

static void get_seat_status(wl_client *client, wl_resource *manager, uint32_t id,
			    wl_resource *seat_resource)
{
	auto *status = new SeatStatus{};
	int version = std::min(wl_resource_get_version(manager),
			       zriver_seat_status_v1_interface.version);
	status->resource = create_resource(client, &zriver_seat_status_v1_interface, version, id,
					   &seat_status_implementation, status);
	if (!status->resource)
		return;
	seat_statuses.push_back(status);
	send_seat_status(status);
}

static const struct zriver_status_manager_v1_interface status_manager_implementation = {
	handle_destroy_request,
	get_output_status,
	get_seat_status,
};

static void bind_status_manager(wl_client *client, void *data, uint32_t version, uint32_t id)
{
	wl_resource *resource = wl_resource_create(client, &zriver_status_manager_v1_interface,
						   version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &status_manager_implementation, nullptr, nullptr);
}

/* ---- river-control: clicks on tags ---- */

static void add_argument(wl_client *client, wl_resource *resource, const char *argument)
{
	auto *control = static_cast<Control *>(wl_resource_get_user_data(resource));
	if (control->arguments.size() < 8)
		control->arguments.push_back(argument);
}

/* Returns null on success, else why it failed. */
static const char *run_tag_command(const std::vector<std::string> &arguments)
{
	if (arguments.empty())
		return "no command";
	const std::string &command = arguments[0];
	bool show = command == "set-focused-tags" || command == "toggle-focused-tags";
	bool move = command == "set-view-tags" || command == "toggle-view-tags";
	if (!show && !move)
		return "waymacs only knows set/toggle-focused-tags and set/toggle-view-tags";
	if (arguments.size() < 2)
		return "no tags given";
	unsigned long tags = strtoul(arguments[1].c_str(), nullptr, 10);
	if (!tags)
		return "no tag set";
	if (!ipc_connected())
		return "Emacs is not connected";
	int number = __builtin_ctzl(tags) + 1;	/* the lowest tag */
	ipc_send(show ? "(workspace-switch :number %d)" : "(workspace-move :number %d)", number);
	return nullptr;
}

static void run_command(wl_client *client, wl_resource *resource, wl_resource *seat,
			uint32_t callback_id)
{
	auto *control = static_cast<Control *>(wl_resource_get_user_data(resource));
	wl_resource *callback =
		wl_resource_create(client, &zriver_command_callback_v1_interface, 1, callback_id);
	if (!callback) {
		wl_client_post_no_memory(client);
		return;
	}
	const char *failure = run_tag_command(control->arguments);
	if (failure)
		zriver_command_callback_v1_send_failure(callback, failure);
	else
		zriver_command_callback_v1_send_success(callback, "");
	wl_resource_destroy(callback);	/* both events are destructors */
	control->arguments.clear();
}

static const struct zriver_control_v1_interface control_implementation = {
	handle_destroy_request,
	add_argument,
	run_command,
};

static void bind_control(wl_client *client, void *data, uint32_t version, uint32_t id)
{
	create_resource(client, &zriver_control_v1_interface, version, id, &control_implementation,
			new Control);
}

/* ---- taskbar (wlr-foreign-toplevel-management) ---- */

void bar_view_mapped(View *app)
{
	if (app->is_frame || app->taskbar_handle)
		return;
	wlr_foreign_toplevel_handle_v1 *handle =
		wlr_foreign_toplevel_handle_v1_create(server.taskbar_manager);
	app->taskbar_handle = handle;
	app->taskbar_activate.listen(&handle->events.request_activate, [app](void *) {
		view_request_activate(app);
	});
	app->taskbar_close.listen(&handle->events.request_close, [app](void *) {
		if (!app->is_frame)
			app->close(false);
	});
	app->taskbar_fullscreen.listen(&handle->events.request_fullscreen, [app](void *data) {
		auto *event = static_cast<wlr_foreign_toplevel_handle_v1_fullscreen_event *>(data);
		if (app->mapped)
			view_set_fullscreen(app, event->fullscreen);
	});
	app->taskbar_destroy.listen(&handle->events.destroy, [app](void *) {
		app->taskbar_activate.stop();
		app->taskbar_close.stop();
		app->taskbar_fullscreen.stop();
		app->taskbar_destroy.stop();
		app->taskbar_handle = nullptr;
		app->taskbar_output = nullptr;
	});
	bar_view_updated(app);
}

void bar_view_unmapped(View *app)
{
	if (app->taskbar_handle)
		wlr_foreign_toplevel_handle_v1_destroy(app->taskbar_handle);
}

void bar_view_updated(View *app)
{
	if (!app->taskbar_handle)
		return;
	wlr_foreign_toplevel_handle_v1_set_title(app->taskbar_handle, app->title());
	wlr_foreign_toplevel_handle_v1_set_app_id(app->taskbar_handle, app->app_id());
}

static void update_taskbar(View *app)
{
	wlr_foreign_toplevel_handle_v1 *handle = app->taskbar_handle;
	bool activated = server.focused_view == app;
	if (activated != !!(handle->state & WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED))
		wlr_foreign_toplevel_handle_v1_set_activated(handle, activated);
	if (app->fullscreen != !!(handle->state & WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN))
		wlr_foreign_toplevel_handle_v1_set_fullscreen(handle, app->fullscreen);
	Output *output = app->frame ? app->frame->output : nullptr;
	auto *wlr_output = output ? output->wlr_output : nullptr;
	if (wlr_output != app->taskbar_output) {
		if (app->taskbar_output)
			wlr_foreign_toplevel_handle_v1_output_leave(handle, app->taskbar_output);
		if (wlr_output)
			wlr_foreign_toplevel_handle_v1_output_enter(handle, wlr_output);
		app->taskbar_output = wlr_output;
	}
}

/* ---- everything ---- */

/* Called by arrange(): send what changed. */
void bar_update()
{
	for (OutputStatus *status : output_statuses)
		send_output_status(status);
	for (SeatStatus *status : seat_statuses)
		send_seat_status(status);
	for (View *view : server.views)
		if (view->taskbar_handle)
			update_taskbar(view);
}

/* An output is going away: forget pointers to it. */
void bar_output_destroyed(struct wlr_output *wlr_output)
{
	for (OutputStatus *status : output_statuses)
		if (status->wlr_output == wlr_output)
			status->wlr_output = nullptr;
	for (SeatStatus *status : seat_statuses)
		if (status->sent_output == wlr_output)
			status->sent_output = nullptr;
	for (View *view : server.views)
		if (view->taskbar_output == wlr_output)
			view->taskbar_output = nullptr;
}

/* Emacs (re)connected. */
void bar_hello()
{
	announced_bar = -1;
	announce_bar();
}

void bar_init()
{
	wl_global_create(server.display, &zriver_status_manager_v1_interface, 4, nullptr,
			 bind_status_manager);
	wl_global_create(server.display, &zriver_control_v1_interface, 1, nullptr, bind_control);
	server.taskbar_manager = wlr_foreign_toplevel_manager_v1_create(server.display);
}
