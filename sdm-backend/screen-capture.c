/*
*    Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
*
*    Redistribution and use in source and binary forms, with or without
*    modification, are permitted provided that the following conditions are
*    met:
*    * Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above
*    copyright notice, this list of conditions and the following
*    disclaimer in the documentation and/or other materials provided
*    with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its
*    contributors may be used to endorse or promote products derived
*    from this software without specific prior written permission.

*    THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
*    WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
*    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
*    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
*    BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
*    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
*    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
*    BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
*    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
*    OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
*    IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*    Changes from Qualcomm Innovation Center are provided under the following license:
*    Copyright (c) 2022, 2024 Qualcomm Innovation Center, Inc. All rights reserved.
*    SPDX-License-Identifier: BSD-3-Clause-Clear
*/


#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <libweston/libweston.h>
#include "gbm_priv.h"
#include "gbm-buffer-backend.h"
#include <libweston-private/linux-dmabuf.h>
#include "screen-capture.h"
#include "gbm-buffer-backend-server-protocol.h"
#include "screen-capture-server-protocol.h"
#include <compositor-sdm-output.h>

extern struct gbm_buffer_backend_c_interface gbm_buffer_backend_c_interface;

static struct drm_output *
screen_capture_create_virtual_display(struct weston_output *mirror_output)
{
	struct drm_output *output = NULL;

	output = zalloc(sizeof *output);
	if (output == NULL)
		goto err;

	//TODO: Create SDM virtual display

	/* If mirror output can be pluggable in future, update output->base in drm_assign_planes */
	output->base = *mirror_output;
	return output;

err:
	return NULL;
}


static int
screen_capture_destroy_virtual_display(struct drm_output *output)
{
	if (output == NULL)
		goto err;

	//TODO: Destroy SDM virtual display

	return 0;

err:
	return -1;
}

static void
screen_capture_create_screen(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *output_resource,
		uint32_t width,
		uint32_t height)
{
	struct screen_capture *screen_cap = NULL;
	struct weston_head *mirror_head;
	struct weston_output *mirror_output;
	struct drm_backend *b;

	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_create_screen::Invoked\n");

	screen_cap = wl_resource_get_user_data(resource);
	if (!screen_cap) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "screen capture is null!");
		screen_capture_send_failed(resource);
		return;
	}

	if (screen_cap->virtual_output) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "screen is already created!");
		screen_capture_send_failed(resource);
		return;
	}

	mirror_head = wl_resource_get_user_data(output_resource);
	mirror_output = mirror_head->output;
	screen_cap->virtual_output = screen_capture_create_virtual_display(mirror_output);
	if (!screen_cap->virtual_output) {
		SC_PROTOCOL_LOG(SC_LOG_ERR,"Error! can't create SDM virtual display\n");
		screen_capture_send_failed(resource);
		return;
	}

	screen_cap->mirror_output = mirror_output;
	screen_cap->mirror_output_id = mirror_output->id;
	screen_cap->width = width;
	screen_cap->height = height;
	b = (struct drm_backend *)screen_cap->compositor->primary_backend;
	b->screen_cap = screen_cap;

	screen_capture_send_created(resource);
	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_create_screen::Exited\n");
}

static void
wait_for_release(struct screen_capture_buffer *cap_buf)
{
	int error = 0;
	int fence = cap_buf->fence_id;

	if (fence != -1) {
		struct pollfd poll_fd = {0};
		poll_fd.fd = fence;
		poll_fd.events = POLLIN;
		error = poll(&poll_fd, 1, 1000);
		if (error <= 0) {
			SC_PROTOCOL_LOG(SC_LOG_ERR,"fail to wait for display WB2 composition!\n");
			/* What happens if poll fails? */
		} else {
			close(fence);
		}
	}
}

static void
screen_capture_exit(struct screen_capture *screen_cap)
{
	struct drm_backend *b = (struct drm_backend *)screen_cap->compositor->primary_backend;
	struct screen_capture_buffer *cap_buf, *next;

	/* Clear those buffers which have not been consumed yet. */
	wl_list_for_each_safe(cap_buf, next, &screen_cap->attached_buf_list, link) {
		wl_list_remove(&cap_buf->link);
		weston_buffer_reference(&cap_buf->buf_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);
		free(cap_buf);
	}

	/* Wait until all capture buffers are not consumed any more. */
	if (screen_cap->current &&
			screen_cap->current != screen_cap->next) {
		wait_for_release(screen_cap->current);
		/* TODO: handle display WB2 composition */
		weston_buffer_reference(&screen_cap->current->buf_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);
		free(screen_cap->current);
		screen_cap->current = NULL;
	}
	if (screen_cap->next) {
		wait_for_release(screen_cap->next);
		weston_buffer_reference(&screen_cap->next->buf_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);
		free(screen_cap->next);
		screen_cap->next = NULL;
	}

	if (screen_cap->virtual_output) {
		if (screen_capture_destroy_virtual_display(screen_cap->virtual_output)) {
			SC_PROTOCOL_LOG(SC_LOG_ERR,"fail to destroy virtual display\n");
		}
		free(screen_cap->virtual_output);
		screen_cap->virtual_output = NULL;
	}

	b->screen_cap = NULL;
}

static void
screen_capture_destroy_screen(struct wl_client *client,
		struct wl_resource *resource)
{
	struct screen_capture *screen_cap = NULL;

	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_destroy_screen::Invoked\n");

	screen_cap = wl_resource_get_user_data(resource);
	if (!screen_cap) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "screen capture is null!");
		screen_capture_send_destroyed(resource);
		return;
	}

	if (!screen_cap->virtual_output) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "screen is already destroyed!");
		screen_capture_send_destroyed(resource);
		return;
	}

	screen_capture_exit(screen_cap);

	screen_capture_send_destroyed(resource);
	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_destroy_screen::Exited\n");
}

static void
screen_capture_start(struct wl_client *client,
		struct wl_resource *resource)
{
	struct screen_capture *screen_cap = NULL;

	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_start::Invoked\n");
	screen_cap = wl_resource_get_user_data(resource);
	if (!screen_cap) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "screen capture is null!");
		return;
	}

	screen_cap->enabled = true;
	screen_capture_send_started(resource);

	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_start::Exited\n");
}

static bool
is_capture_ready(struct screen_capture *screen_cap, struct weston_output *output)
{
	if (!screen_cap)
		return false;

	return (screen_cap->enabled &&
			screen_cap->mirror_output_id == output->id);
}

static void
screen_capture_attach(struct weston_compositor *compositor,
		struct weston_buffer *buffer)
{
	struct gbm_buffer *gbm_buf = NULL;
	struct drm_backend *b = (struct drm_backend *)compositor->primary_backend;
	struct screen_capture *screen_cap = b->screen_cap;
	struct screen_capture_buffer *cap_buf;

	/* Skip attach buffer if screen capture is not enabled */
	if (!screen_cap || !screen_cap->enabled)
		return;

	/* Only support GBM buffer now. */
	if(!buffer ||
		wl_shm_buffer_get(buffer->resource) ||
		linux_dmabuf_buffer_get(compositor, buffer->resource)) {
		return;
	}

	/* Screen capture buffer can't be NULL */
	gbm_buf = gbm_buffer_backend_c_interface.buffer_get(buffer->resource);
	if (!gbm_buf) {
		return;
	}

	wl_list_for_each(cap_buf, &screen_cap->attached_buf_list, link) {
		/* skip buffer which have already existed in the buffer list, this situation happends on
		 * gl_renderer_create_surface which call gl_renderer_attach first, weston_surface_attach
		 * call gl_renderer_attach second time for the same buffer.
		 */
		if(cap_buf->buffer == buffer) {
			SC_PROTOCOL_LOG(SC_LOG_DBG,"attach the existed capture buffer!\n");
			return;
		}
	}

	if (gbm_buf->flags & GBM_BUFFER_PARAMS_FLAGS_SCREEN_CAPTURE) {
		struct screen_capture_buffer *capture_buf = NULL;

		if (buffer->width != screen_cap->width ||
				buffer->height != screen_cap->height) {
			SC_PROTOCOL_LOG(SC_LOG_ERR,"invalid w/h of capture buffer!\n");
			printf("buffer: w=%d, h=%d; screen: w=%d, h=%d\n", buffer->width, buffer->height, screen_cap->width, screen_cap->height);
			return;
		}

		capture_buf = zalloc(sizeof *capture_buf);
		if (capture_buf == NULL) {
			SC_PROTOCOL_LOG(SC_LOG_ERR,"no memory to create capture buffer!\n");
			return;
		}

		capture_buf->buffer = buffer;
		capture_buf->fence_id = -1;
		wl_list_init(&capture_buf->link);
		wl_list_insert(screen_cap->attached_buf_list.prev, &capture_buf->link);

		SC_PROTOCOL_LOG(SC_LOG_DBG,"screen capture buffer is attached!\n");
	}
}

static bool
is_screen_capture_buffer(struct weston_buffer *buffer)
{

	if (buffer) {
		struct gbm_buffer *gbm_buf = gbm_buffer_backend_c_interface.buffer_get(buffer->resource);

		if (gbm_buf &&
				gbm_buf->flags & GBM_BUFFER_PARAMS_FLAGS_SCREEN_CAPTURE) {
			return true;
		}
	}

	return false;
}

static bool
is_screen_capture_view(struct weston_view *ev)
{
	if (ev && ev->is_capture_view)
		return true;

	if (ev && ev->surface && ev->surface->buffer_ref.buffer) {
		struct gbm_buffer *gbm_buf =
				gbm_buffer_backend_c_interface.buffer_get(ev->surface->buffer_ref.buffer->resource);

		if (gbm_buf &&
				gbm_buf->flags & GBM_BUFFER_PARAMS_FLAGS_SCREEN_CAPTURE) {
			/*
			 * ev->surface->buffer_ref.buffer will be NULL during close
			 * animation of screen capture application, if no hint is
			 * stored, the last frame of screen capture application will be
			 * used as texture during composition, which is not expected.
			 */
			ev->is_capture_view = true;
			return true;
		}
	}

	return false;
}

static void
screen_capture_stop(struct wl_client *client,
		struct wl_resource *resource)
{
	struct screen_capture *screen_cap = NULL;

	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_stop::Invoked\n");

	screen_cap = wl_resource_get_user_data(resource);
	if (!screen_cap) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "screen capture is null!");
		return;
	}

	screen_cap->enabled = false;
	screen_capture_send_stopped(resource);

	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_stop::Exited\n");
}

static void
screen_capture_destroy(struct wl_client *client,
		struct wl_resource *resource)
{
	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_destroy::Invoked\n");

	wl_resource_destroy(resource);

	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_destroy::Exited\n");
}

static void
destroy_screen_capture(struct wl_resource *resource)
{
	struct screen_capture *screen_cap = NULL;

	screen_cap = wl_resource_get_user_data(resource);
	if (screen_cap) {
		/* call exit again to avoid expected client crash */
		screen_capture_exit(screen_cap);
		free(screen_cap);
	}
	wl_resource_set_user_data(resource, NULL);
}

static const struct screen_capture_interface screen_capture_implementation = {
	screen_capture_destroy,
	screen_capture_create_screen,
	screen_capture_destroy_screen,
	screen_capture_start,
	screen_capture_stop
};

static void screen_capture_init(struct screen_capture *sc, struct weston_compositor *compositor)
{
	sc->compositor = compositor;

	sc->enabled = false;
	sc->fallback_gpu = false;
	sc->mirror_output_id = 0; /* mirror the primary display by default */
	sc->virtual_output = NULL;
	sc->view = NULL;
	sc->next = sc->current = NULL;

	wl_list_init(&sc->attached_buf_list);
}

static void
bind_screen_capture(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	struct weston_compositor *compositor = data;
	struct wl_resource *resource;
	struct screen_capture *screen_cap = NULL;
	struct drm_backend *b = (struct drm_backend *)compositor->primary_backend;

	SC_PROTOCOL_LOG(SC_LOG_DBG,"bind_screen_capture::Invoked\n");

	/* User needs to guarantee only one instance is running */
	if (b->screen_cap) {
		SC_PROTOCOL_LOG(SC_LOG_ERR,"another client has already started screen capture!\n");
		wl_client_post_no_memory(client);
		return;
	}

	screen_cap = zalloc(sizeof *screen_cap);
	if (screen_cap == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	resource = wl_resource_create(client, &screen_capture_interface,
					version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		free(screen_cap);
		return;
	}

	screen_capture_init(screen_cap, compositor);

	wl_resource_set_implementation(resource,
			&screen_capture_implementation,
			screen_cap, destroy_screen_capture);

	SC_PROTOCOL_LOG(SC_LOG_DBG,"bind_screen_capture::Exited\n");
}

static int
screen_capture_setup(struct weston_compositor *compositor)
{
	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_setup::Invoked\n");

	if (!wl_global_create(compositor->wl_display,
				&screen_capture_interface, 1,
				compositor, bind_screen_capture)) {
		return -1;
	}
	SC_PROTOCOL_LOG(SC_LOG_DBG,"screen_capture_setup::Exited\n");

	return 0;
}

WL_EXPORT struct screen_capture_c_interface screen_capture_c_interface = {
	.setup = screen_capture_setup,
	.is_screen_capture_buffer = is_screen_capture_buffer,
	.is_screen_capture_view = is_screen_capture_view,
	.attach = screen_capture_attach,
	.is_capture_ready = is_capture_ready,
};
