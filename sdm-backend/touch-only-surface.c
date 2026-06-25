/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdlib.h>
#include <wayland-server-core.h>
#include <libweston/libweston.h>

#include "touch-only-surface-server-protocol.h"

struct touch_only_surface_global {
	struct wl_global *global;
	struct weston_compositor *compositor;
};

static void
touch_only_surface_set_touch_only(struct wl_client *client,
				  struct wl_resource *resource,
				  struct wl_resource *surface_resource,
				  int32_t enabled)
{
	struct weston_surface *surface;

	if (!surface_resource)
		return;

	surface = wl_resource_get_user_data(surface_resource);
	if (!surface) {
		wl_resource_post_error(resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface is null");
		return;
	}

	surface->is_touch_only = (enabled != 0);
}

static const struct touch_only_surface_interface
touch_only_surface_implementation = {
	touch_only_surface_set_touch_only,
};

static void
bind_touch_only_surface(struct wl_client *client,
			void *data,
			uint32_t version,
			uint32_t id)
{
	struct touch_only_surface_global *global = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &touch_only_surface_interface,
				      version,
				      id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource,
				       &touch_only_surface_implementation,
				       global,
				       NULL);
}

int
touch_only_surface_init(struct weston_compositor *ec)
{
	struct touch_only_surface_global *global;

	global = zalloc(sizeof *global);
	if (!global)
		return -1;

	global->compositor = ec;
	global->global = wl_global_create(ec->wl_display,
					  &touch_only_surface_interface,
					  1,
					  global,
					  bind_touch_only_surface);
	if (!global->global) {
		free(global);
		return -1;
	}

	return 0;
}
