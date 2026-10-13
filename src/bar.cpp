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

struct output_status {
	struct wl_list link;		/* server.output_statuses */
	struct wl_resource *resource;
	struct wlr_output *wlr_output;
	bool sent;
	uint32_t sent_focused_tags;
	struct wl_array sent_view_tags;
};

struct seat_status {
	struct wl_list link;		/* server.seat_statuses */
	struct wl_resource *resource;
	bool sent;
	struct wlr_output *sent_output;
	char *sent_title;
};

#define MAX_COMMAND_ARGUMENTS 8

struct control {
	int argument_count;
	char *arguments[MAX_COMMAND_ARGUMENTS];
};

/* ---- river-status ---- */

static uint32_t workspace_tag(struct view *frame)
{
	if (!frame || frame->workspace < 1 || frame->workspace > 32)
		return 0;
	return 1u << (frame->workspace - 1);
}

/* OUTPUT's shown workspace, and the workspace of every window on it */
static void output_tags(struct output *output, uint32_t *focused_tags, struct wl_array *view_tags)
{
	*focused_tags = output ? workspace_tag(output->active_frame) : 0;
	wl_array_init(view_tags);
	if (!output)
		return;
	/* workspaces not shown yet belong to no output: list them on the
	 * focused one, where they will appear */
	if (output == focused_output())
		for (int bit = 0; bit < 32; bit++)
			if (server.unshown_workspace_tags & (1u << bit)) {
				uint32_t *slot = (uint32_t *)wl_array_add(view_tags, sizeof *slot);
				if (slot)
					*slot = 1u << bit;
			}
	struct view *view;
	wl_list_for_each(view, &server.views, link) {
		struct view *frame = view->is_frame ? view : view->frame;
		uint32_t tag = workspace_tag(frame);
		if (!view->mapped || !frame || frame->output != output || !tag)
			continue;
		uint32_t *slot = (uint32_t *)wl_array_add(view_tags, sizeof *slot);
		if (slot)
			*slot = tag;
	}
}

static void send_output_status(struct output_status *status)
{
	struct output *output = status->wlr_output ?
		(struct output *)status->wlr_output->data : NULL;
	uint32_t focused_tags;
	struct wl_array view_tags;
	output_tags(output, &focused_tags, &view_tags);
	if (!status->sent || focused_tags != status->sent_focused_tags)
		zriver_output_status_v1_send_focused_tags(status->resource, focused_tags);
	if (!status->sent || view_tags.size != status->sent_view_tags.size ||
	    memcmp(view_tags.data, status->sent_view_tags.data, view_tags.size) != 0)
		zriver_output_status_v1_send_view_tags(status->resource, &view_tags);
	if (!status->sent && wl_resource_get_version(status->resource) >=
	    ZRIVER_OUTPUT_STATUS_V1_URGENT_TAGS_SINCE_VERSION)
		zriver_output_status_v1_send_urgent_tags(status->resource, 0);
	status->sent = true;
	status->sent_focused_tags = focused_tags;
	wl_array_release(&status->sent_view_tags);
	status->sent_view_tags = view_tags;
}

static struct wl_resource *client_output_resource(struct wlr_output *wlr_output,
						  struct wl_client *client)
{
	struct wl_resource *resource;
	wl_resource_for_each(resource, &wlr_output->resources)
		if (wl_resource_get_client(resource) == client)
			return resource;
	return NULL;
}

static void send_seat_status(struct seat_status *status)
{
	struct wl_client *client = wl_resource_get_client(status->resource);
	struct output *output = focused_output();
	struct wlr_output *wlr_output = output ? output->wlr_output : NULL;
	if (!status->sent || wlr_output != status->sent_output) {
		struct wl_resource *resource;
		if (status->sent_output &&
		    (resource = client_output_resource(status->sent_output, client)))
			zriver_seat_status_v1_send_unfocused_output(status->resource, resource);
		if (wlr_output && (resource = client_output_resource(wlr_output, client)))
			zriver_seat_status_v1_send_focused_output(status->resource, resource);
		status->sent_output = wlr_output;
	}
	struct view *view = server.focused_view;
	const char *title = view && view->mapped ? view_title(view) : "";
	if (!strncmp(title, "waymacs-frame-", 14))	/* a frame being paired */
		title = "";
	if (!status->sent || !status->sent_title || strcmp(title, status->sent_title)) {
		zriver_seat_status_v1_send_focused_view(status->resource, title);
		free(status->sent_title);
		status->sent_title = strdup(title);
	}
	if (!status->sent && wl_resource_get_version(status->resource) >=
	    ZRIVER_SEAT_STATUS_V1_MODE_SINCE_VERSION)
		zriver_seat_status_v1_send_mode(status->resource, "normal");
	status->sent = true;
}

/* Emacs hides its own workspace list while a bar shows them. */
static void announce_bar(void)
{
	static int announced = -1;
	int present = !wl_list_empty(&server.output_statuses);
	if (present != announced && ipc_connected()) {
		ipc_send("(bar :workspaces %s)", present ? "t" : "nil");
		announced = present;
	}
}

static void handle_destroy_request(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void output_status_destroyed(struct wl_resource *resource)
{
	struct output_status *status = (struct output_status *)wl_resource_get_user_data(resource);
	wl_list_remove(&status->link);
	wl_array_release(&status->sent_view_tags);
	free(status);
	announce_bar();
}

static void seat_status_destroyed(struct wl_resource *resource)
{
	struct seat_status *status = (struct seat_status *)wl_resource_get_user_data(resource);
	wl_list_remove(&status->link);
	free(status->sent_title);
	free(status);
}

static const struct zriver_output_status_v1_interface output_status_implementation = {
	handle_destroy_request,
};

static const struct zriver_seat_status_v1_interface seat_status_implementation = {
	handle_destroy_request,
};

static void get_output_status(struct wl_client *client, struct wl_resource *manager,
			      uint32_t id, struct wl_resource *output_resource)
{
	struct output_status *status = (struct output_status *)calloc(1, sizeof *status);
	status->resource = wl_resource_create(client, &zriver_output_status_v1_interface,
					      wl_resource_get_version(manager), id);
	if (!status->resource) {
		free(status);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(status->resource, &output_status_implementation, status,
				       output_status_destroyed);
	status->wlr_output = wlr_output_from_resource(output_resource);
	wl_array_init(&status->sent_view_tags);
	wl_list_insert(&server.output_statuses, &status->link);
	send_output_status(status);
	announce_bar();
}

static void get_seat_status(struct wl_client *client, struct wl_resource *manager,
			    uint32_t id, struct wl_resource *seat_resource)
{
	struct seat_status *status = (struct seat_status *)calloc(1, sizeof *status);
	int version = wl_resource_get_version(manager);
	if (version > zriver_seat_status_v1_interface.version)
		version = zriver_seat_status_v1_interface.version;
	status->resource = wl_resource_create(client, &zriver_seat_status_v1_interface,
					      version, id);
	if (!status->resource) {
		free(status);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(status->resource, &seat_status_implementation, status,
				       seat_status_destroyed);
	wl_list_insert(&server.seat_statuses, &status->link);
	send_seat_status(status);
}

static const struct zriver_status_manager_v1_interface status_manager_implementation = {
	handle_destroy_request,
	get_output_status,
	get_seat_status,
};

static void bind_status_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &zriver_status_manager_v1_interface,
							  version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &status_manager_implementation, NULL, NULL);
}

/* ---- river-control: clicks on tags ---- */

static void clear_arguments(struct control *control)
{
	for (int i = 0; i < control->argument_count; i++)
		free(control->arguments[i]);
	control->argument_count = 0;
}

static void control_destroyed(struct wl_resource *resource)
{
	struct control *control = (struct control *)wl_resource_get_user_data(resource);
	clear_arguments(control);
	free(control);
}

static void add_argument(struct wl_client *client, struct wl_resource *resource,
			 const char *argument)
{
	struct control *control = (struct control *)wl_resource_get_user_data(resource);
	if (control->argument_count < MAX_COMMAND_ARGUMENTS)
		control->arguments[control->argument_count++] = strdup(argument);
}

/* Returns NULL on success, else why it failed. */
static const char *run_tag_command(struct control *control)
{
	if (control->argument_count < 1)
		return "no command";
	const char *command = control->arguments[0];
	bool show = !strcmp(command, "set-focused-tags") || !strcmp(command, "toggle-focused-tags");
	bool move = !strcmp(command, "set-view-tags") || !strcmp(command, "toggle-view-tags");
	if (!show && !move)
		return "waymacs only knows set/toggle-focused-tags and set/toggle-view-tags";
	if (control->argument_count < 2)
		return "no tags given";
	unsigned long tags = strtoul(control->arguments[1], NULL, 10);
	if (!tags)
		return "no tag set";
	if (!ipc_connected())
		return "Emacs is not connected";
	int number = __builtin_ctzl(tags) + 1;	/* the lowest tag */
	ipc_send(show ? "(workspace-switch :number %d)" : "(workspace-move :number %d)", number);
	return NULL;
}

static void run_command(struct wl_client *client, struct wl_resource *resource,
			struct wl_resource *seat, uint32_t callback_id)
{
	struct control *control = (struct control *)wl_resource_get_user_data(resource);
	struct wl_resource *callback =
		wl_resource_create(client, &zriver_command_callback_v1_interface, 1, callback_id);
	if (!callback) {
		wl_client_post_no_memory(client);
		return;
	}
	const char *failure = run_tag_command(control);
	if (failure)
		zriver_command_callback_v1_send_failure(callback, failure);
	else
		zriver_command_callback_v1_send_success(callback, "");
	wl_resource_destroy(callback);	/* both events are destructors */
	clear_arguments(control);
}

static const struct zriver_control_v1_interface control_implementation = {
	handle_destroy_request,
	add_argument,
	run_command,
};

static void bind_control(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client, &zriver_control_v1_interface,
							  version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	struct control *control = (struct control *)calloc(1, sizeof *control);
	wl_resource_set_implementation(resource, &control_implementation, control,
				       control_destroyed);
}

/* ---- taskbar (wlr-foreign-toplevel-management) ---- */

static void taskbar_request_activate(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, taskbar_activate);
	view_request_activate(view);
}

static void taskbar_request_close(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, taskbar_close);
	view_close(view, false);
}

static void taskbar_request_fullscreen(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, taskbar_fullscreen);
	auto *event = (struct wlr_foreign_toplevel_handle_v1_fullscreen_event *)data;
	if (view->mapped)
		view_set_fullscreen(view, event->fullscreen);
}

static void taskbar_handle_destroyed(struct wl_listener *listener, void *data)
{
	struct view *view = wl_container_of(listener, view, taskbar_destroy);
	stop_listening(&view->taskbar_activate, &view->taskbar_close,
		       &view->taskbar_fullscreen, &view->taskbar_destroy);
	view->taskbar_handle = NULL;
	view->taskbar_output = NULL;
}

void bar_view_mapped(struct view *app)
{
	if (app->is_frame || app->taskbar_handle)
		return;
	struct wlr_foreign_toplevel_handle_v1 *handle =
		wlr_foreign_toplevel_handle_v1_create(server.taskbar_manager);
	app->taskbar_handle = handle;
	LISTEN(&handle->events.request_activate, &app->taskbar_activate, taskbar_request_activate);
	LISTEN(&handle->events.request_close, &app->taskbar_close, taskbar_request_close);
	LISTEN(&handle->events.request_fullscreen, &app->taskbar_fullscreen,
	       taskbar_request_fullscreen);
	LISTEN(&handle->events.destroy, &app->taskbar_destroy, taskbar_handle_destroyed);
	bar_view_updated(app);
}

void bar_view_unmapped(struct view *app)
{
	if (app->taskbar_handle)
		wlr_foreign_toplevel_handle_v1_destroy(app->taskbar_handle);
}

void bar_view_updated(struct view *app)
{
	if (!app->taskbar_handle)
		return;
	wlr_foreign_toplevel_handle_v1_set_title(app->taskbar_handle, view_title(app));
	wlr_foreign_toplevel_handle_v1_set_app_id(app->taskbar_handle, view_app_id(app));
}

static void update_taskbar(struct view *app)
{
	struct wlr_foreign_toplevel_handle_v1 *handle = app->taskbar_handle;
	bool activated = server.focused_view == app;
	if (activated != !!(handle->state & WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED))
		wlr_foreign_toplevel_handle_v1_set_activated(handle, activated);
	if (app->fullscreen != !!(handle->state & WLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN))
		wlr_foreign_toplevel_handle_v1_set_fullscreen(handle, app->fullscreen);
	struct output *output = app->frame ? app->frame->output : NULL;
	struct wlr_output *wlr_output = output ? output->wlr_output : NULL;
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
void bar_update(void)
{
	struct output_status *output_status;
	wl_list_for_each(output_status, &server.output_statuses, link)
		send_output_status(output_status);
	struct seat_status *seat_status;
	wl_list_for_each(seat_status, &server.seat_statuses, link)
		send_seat_status(seat_status);
	struct view *view;
	wl_list_for_each(view, &server.views, link)
		if (view->taskbar_handle)
			update_taskbar(view);
}

/* An output is going away: forget pointers to it. */
void bar_output_destroyed(struct wlr_output *wlr_output)
{
	struct output_status *output_status;
	wl_list_for_each(output_status, &server.output_statuses, link)
		if (output_status->wlr_output == wlr_output)
			output_status->wlr_output = NULL;
	struct seat_status *seat_status;
	wl_list_for_each(seat_status, &server.seat_statuses, link)
		if (seat_status->sent_output == wlr_output)
			seat_status->sent_output = NULL;
	struct view *view;
	wl_list_for_each(view, &server.views, link)
		if (view->taskbar_output == wlr_output)
			view->taskbar_output = NULL;
}

/* Emacs (re)connected. */
void bar_hello(void)
{
	if (ipc_connected())
		ipc_send("(bar :workspaces %s)",
			 wl_list_empty(&server.output_statuses) ? "nil" : "t");
}

void bar_init(void)
{
	wl_list_init(&server.output_statuses);
	wl_list_init(&server.seat_statuses);
	wl_global_create(server.display, &zriver_status_manager_v1_interface, 4, NULL,
			 bind_status_manager);
	wl_global_create(server.display, &zriver_control_v1_interface, 1, NULL, bind_control);
	server.taskbar_manager = wlr_foreign_toplevel_manager_v1_create(server.display);
}
