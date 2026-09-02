/*
* Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*    * Redistributions of source code must retain the above copyright
*      notice, this list of conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above
*      copyright notice, this list of conditions and the following
*      disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its
*      contributors may be used to endorse or promote products derived
*      from this software without specific prior written permission.

* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* Copyright © 2008-2011 Kristian Høgsberg
* Copyright © 2011 Intel Corporation
*
* Permission to use, copy, modify, distribute, and sell this software and
* its documentation for any purpose is hereby granted without fee, provided
* that the above copyright notice appear in all copies and that both that
* copyright notice and this permission notice appear in supporting
* documentation, and that the name of the copyright holders not be used in
* advertising or publicity pertaining to distribution of the software
* without specific, written prior permission.  The copyright holders make
* no representations about the suitability of this software for any
* purpose.It is provided "as is" without express or implied warranty.
*
* THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
* SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
* FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
* SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
* RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
* CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
* CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*
* Changes from Qualcomm Technologies, Inc. are provided under the following license:
* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/vt.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <dlfcn.h>
#include <time.h>
#include <linux/sync_file.h>
#include <pthread.h>
#include <cutils/properties.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <gbm.h>
#include <libudev.h>
#include <libweston/libweston.h>
#include <libweston/backend-drm.h>
#include <libweston/weston-log.h>
#include <libweston/config-parser.h>

#include "shared/helpers.h"
#include "shared/timespec-util.h"
#include "shared/weston-egl-ext.h"
#include "shared/string-helpers.h"
#include <libweston-private/libbacklight.h>
#include <libweston-private/gl-renderer.h>
#include <libweston-private/linux-dmabuf.h>
#include <libweston-private/launcher-util.h>
#include <libweston-private/pixman-renderer.h>

#include "touch-only-surface-server-protocol.h"

#include <gbm-buffer-backend.h>
#include <screen-capture.h>
#include "../sdm-service/sdm_display_connect.h"
#include <compositor-sdm-output.h>
#include "drm-service/drm_display.h"

#include "pll-server-protocol.h"
#include "presentation-time-server-protocol.h"
#include "gbm-buffer-backend-server-protocol.h"
#include "bootkpi/logging.h"
#include "gbm-buffer-backend.h"

#if defined(ENABLE_RT_SCHEDULE)
#include "amss/compresmgr_client_api.h"
#endif

#ifndef DRM_CAP_TIMESTAMP_MONOTONIC
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#endif

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif

#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

#ifndef GBM_BO_USE_CURSOR
#define GBM_BO_USE_CURSOR GBM_BO_USE_CURSOR_64X64
#endif

#define MAX_CLONED_CONNECTORS 4

static int option_current_mode = 0;
enum {
	DISABLE,
	ENABLE
};

enum output_config {
	OUTPUT_CONFIG_INVALID = 0,
	OUTPUT_CONFIG_OFF,
	OUTPUT_CONFIG_PREFERRED,
	OUTPUT_CONFIG_CURRENT,
	OUTPUT_CONFIG_MODE,
	OUTPUT_CONFIG_MODELINE
};

struct drm_mode {
	struct weston_mode base;
	drmModeModeInfo mode_info;
};

struct drm_parameters {
	int connector;
	int tty;
	int use_pixman;
	const char *seat_id;
};
struct drm_head {
	struct weston_head base;
	struct drm_backend *backend;
	uint32_t connector_id;
	uint32_t display_id;
	struct backlight *backlight;
	drmModeModeInfo inherited_mode;	/**< Original mode on the connector */
	uint32_t inherited_crtc_id;	/**< Original CRTC assignment */
	bool early_display_enable; /* whether hw display enabled in early stage */
	void *early_display_intf;
};

static struct gl_renderer_interface *gl_renderer;

/* sdm service is built as a seperate shared library */
static struct sdm_service_interface *sdm_service;

static const char default_seat[] = "seat0";

pthread_mutex_t full_init_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t full_init_cond = PTHREAD_COND_INITIALIZER;
extern struct gbm_buffer_backend_c_interface gbm_buffer_backend_c_interface;
extern struct screen_capture_c_interface screen_capture_c_interface;

extern int early_renderer_init(struct weston_compositor *ec,
		struct gbm_device * gbm);

typedef int (*touch_only_surface_init_func_t)(struct weston_compositor *ec);

static touch_only_surface_init_func_t touch_only_surface_init_func;

static int
load_touch_only_surface_module(void)
{
	if (touch_only_surface_init_func)
		return 0;

	touch_only_surface_init_func =
		(touch_only_surface_init_func_t)weston_load_module(
			"touch-only-surface.so",
			"touch_only_surface_init",
			LIBWESTON_MODULEDIR);
	if (!touch_only_surface_init_func) {
		weston_log("Failed to load touch-only-surface.so\n");
		return -1;
	}

	return 0;
}

static void
unload_touch_only_surface_module(void)
{
	touch_only_surface_init_func = NULL;
}

static void
drm_output_update_msc(struct drm_output *output, unsigned int seq);

static int create_sdm_display(struct drm_backend *backend, int idx);

static inline struct drm_head *
to_drm_head(struct weston_head *base)
{
	return container_of(base, struct drm_head, base);
}

static inline struct drm_output *
to_drm_output(struct weston_output *base)
{
	return container_of(base, struct drm_output, base);
}

static void
drm_destroy(struct weston_backend *backend);

static inline struct drm_backend *
to_drm_backend(struct weston_compositor *base)
{
	struct weston_backend *backend;

	wl_list_for_each(backend, &base->backend_list, link) {
		if (backend->destroy == drm_destroy)
			return container_of(backend, struct drm_backend, base);
	}

	return NULL;
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
	struct gbm_device *gbm = gbm_bo_get_device(bo);

	weston_buffer_reference(&fb->buffer_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);

	if (fb->ion_fd > -1) {
		close(fb->ion_fd);
	}

	free(data);
}

static struct drm_fb *
drm_fb_create_dumb(struct drm_backend *b, unsigned width, unsigned height)
{
	struct drm_fb *fb;

	struct drm_mode_create_dumb create_arg;
	struct drm_mode_destroy_dumb destroy_arg;
	struct drm_mode_map_dumb map_arg;
	struct drm_prime_handle prime_arg;
	struct gbm_bo *bo;
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	void *map_data = NULL;

	fb = zalloc(sizeof *fb);
	if (!fb)
		return NULL;
	fb->refcnt = 1;

	memset(&create_arg, 0, sizeof create_arg);
	create_arg.bpp = 32;
	create_arg.width = width;
	create_arg.height = height;

	bo = gbm_bo_create(b->gbm, width, height, b->format->format, GBM_BO_USE_SCANOUT);
	if (!bo)
		goto err_fb;

	fb->type = BUFFER_PIXMAN_DUMB;
	fb->handle = gbm_bo_get_handle(bo).u32; //create_arg.handle;
	fb->stride = gbm_bo_get_stride(bo); //create_arg.pitch;
	fb->size = gbm_perform(GBM_PERFORM_GET_BO_SIZE, bo, &(fb->size));//create_arg.size;
	fb->fd = b->drm.fd;

	memset(&prime_arg, 0, sizeof prime_arg);
	prime_arg.handle = fb->handle;

	fb->ion_fd = gbm_bo_get_fd(bo);//prime_arg.fd;
	fb->bo = bo;

	handles[0] = fb->handle;
	pitches[0] = fb->stride;

	weston_log("fb w %d h %d stride %d handle %d drmfd %d\n", width, height, fb->stride, fb->handle, b->drm.fd);
	weston_log("fb map\n");

	fb->map = gbm_bo_map(fb->bo, 0, 0, width, height, GBM_BO_USE_WRITE, &fb->stride, &map_data);
	if (fb->map == MAP_FAILED)
		goto err_add_fb;

	weston_log("fb end\n");
	return fb;

err_add_fb:
err_bo:
	gbm_bo_destroy(bo);
	close(fb->ion_fd);
err_fb:
	free(fb);
	return NULL;
}

/* TODO: Check if necessary for sdm-backend */
struct drm_fb *
drm_fb_ref(struct drm_fb *fb)
{
	fb->refcnt++;
	return fb;
}

static void
drm_fb_destroy_dumb(struct drm_fb *fb)
{
	struct drm_mode_destroy_dumb destroy_arg;

	if (!fb->map)
		return;

	if (fb->fb_id)
		drmModeRmFB(fb->fd, fb->fb_id);

	weston_buffer_reference(&fb->buffer_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);

	munmap(fb->map, fb->size);

	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = fb->handle;
	gbm_bo_destroy(fb->bo);
	close(fb->ion_fd);

	free(fb);
}

static struct drm_fb *
drm_fb_get_from_bo(struct drm_output *output, struct gbm_bo *bo,
		   struct drm_backend *backend)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);

	if (fb)
		return fb;

	fb = zalloc(sizeof *fb);
	if (fb == NULL)
		return NULL;

	fb->bo = bo;

	fb->ion_fd = gbm_bo_get_fd(bo);
	if (fb->ion_fd < 0) {
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}

void
drm_fb_unref(struct drm_fb *fb)
{
	if (!fb)
		return;

	assert(fb->refcnt > 0);
	if (--fb->refcnt > 0)
		return;

	switch (fb->type) {
	case BUFFER_PIXMAN_DUMB:
		drm_fb_destroy_dumb(fb);
		break;
	default:
		assert(NULL);
		break;
	}
}

static void
drm_output_release_fb(struct drm_output *output, struct drm_fb *fb)
{
	if (!fb)
		return;

	if (fb->map &&
			(fb != output->dumb[0] && fb != output->dumb[1])) {
		drm_fb_destroy_dumb(fb);
	} else if (fb->bo) {
		if (fb->is_client_buffer)
			gbm_bo_destroy(fb->bo);
		else
			gbm_surface_release_buffer(output->surface,
						   fb->bo);
	}
}

static void
drm_output_render_gl(struct drm_output *output, pixman_region32_t *damage)
{
	struct drm_backend *b = to_drm_backend(output->base.compositor);
	struct gbm_bo *bo;

	output->base.compositor->renderer->repaint_output(&output->base,
							  damage, NULL);

	bo = gbm_surface_lock_front_buffer(output->surface);
	if (!bo) {
		weston_log("drm_output_render_gl: failed to lock front buffer: %m\n");
		return;
	}

	output->next = drm_fb_get_from_bo(output, bo, b);
	if (!output->next) {
		weston_log("failed to get drm_fb for bo\n");
		gbm_surface_release_buffer(output->surface, bo);
		return;
	}
}

static void
drm_output_render_pixman(struct drm_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *ec = output->base.compositor;

	output->current_image ^= 1;
	output->next = output->dumb[output->current_image];

	ec->renderer->repaint_output(&output->base, damage,
					output->renderbuffer[output->current_image]);
}

static void
drm_output_render(struct drm_output *output, pixman_region32_t *damage)
{
	struct weston_compositor *c = output->base.compositor;
	struct drm_backend *b = to_drm_backend(c);

	if (b->use_pixman)
		drm_output_render_pixman(output, damage);
	else {
		drm_output_render_gl(output, damage);
	}
}


/* Determine the type of vblank synchronization to use for the output.
 *
 * The pipe parameter indicates which CRTC is in use.  Knowing this, we
 * can determine which vblank sequence type to use for it.	Traditional
 * cards had only two CRTCs, with CRTC 0 using no special flags, and
 * CRTC 1 using DRM_VBLANK_SECONDARY.  The first bit of the pipe
 * parameter indicates this.
 *
 * Bits 1-5 of the pipe parameter are 5 bit wide pipe number between
 * 0-31.  If this is non-zero it indicates we're dealing with a
 * multi-gpu situation and we need to calculate the vblank sync
 * using DRM_BLANK_HIGH_CRTC_MASK.
 */
static unsigned int
drm_waitvblank_pipe(struct drm_output *output)
{
	if (output->pipe > 1)
		return (output->pipe << DRM_VBLANK_HIGH_CRTC_SHIFT) &
				DRM_VBLANK_HIGH_CRTC_MASK;
	else if (output->pipe > 0)
		return DRM_VBLANK_SECONDARY;
	else
		return 0;
}

static void destroy_sdm_layer(struct sdm_layer *layer);

static void
destroy_early_layer(struct early_layer *layer)
{
	weston_buffer_reference(&layer->buffer_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);
	wl_list_remove(&layer->link);
	if (layer->bo) {
		gbm_bo_destroy(layer->bo);
	}
	close(layer->sync_handle);
	layer->sync_handle = -1;
	free(layer);
}

static struct early_layer *
create_early_layer(struct weston_paint_node *pnode)
{
	struct early_layer *layer = NULL;
	struct weston_view *ev = pnode->view;

	layer = zalloc(sizeof(*layer));
	if (layer == NULL) {
		weston_log("no enough memory for early layer\n");
		return NULL;
	}

	layer->view = ev;
	layer->pnode = pnode;
	return layer;
}

static int get_fence_timestamp(int fd, struct timespec *ts)
{
	struct sync_file_info *info;
	struct sync_fence_info *fence_info;
	int ret;

	info = calloc(1, sizeof(*info));
	if (!info)
		return -1;

	ret = ioctl(fd, SYNC_IOC_FILE_INFO, info);
	if (ret < 0)
		return ret;

	if (info->num_fences) {
		info->flags = 0;

		fence_info = calloc(info->num_fences, sizeof(*fence_info));
		if (!fence_info) {
			free(info);
			return -1;
		}

		info->sync_fence_info = (uint64_t)fence_info;
		ret = ioctl(fd, SYNC_IOC_FILE_INFO, info);
		if (ret < 0) {
			free(fence_info);
			free(info);
			return ret;
		}
	} else {
		free(info);
		return -1;
	}

	ts->tv_sec = fence_info[0].timestamp_ns / 1000000000LL;
	ts->tv_nsec = fence_info[0].timestamp_ns % 1000000000LL;;

	free(fence_info);
	free(info);
	return 0;
}

static void
on_pageflip(struct drm_output *output);

static int
on_pageflip_vsync(int fd, uint32_t mask, void *data);

static int
retire_fence_cb(int fd, uint32_t mask, void *data)
{
	struct drm_output *output = (struct drm_output *) data;
	struct timespec ts;
	static bool first_fence = true;

	if (first_fence) {
		first_fence = false;
		bootkpi_log_init();
		bootkpi_log_line("W - first frame have been displayed");
	}

	wl_event_source_remove(output->retire_fence_source);
	output->retire_fence_source = NULL;

	if (get_fence_timestamp(output->retire_fence_fd, &ts))
		weston_compositor_read_presentation_clock(output->base.compositor, &ts);

	close(output->retire_fence_fd);
	output->retire_fence_fd = -1;
	output->last_vblank.sec = ts.tv_sec;
	output->last_vblank.usec = ts.tv_nsec / 1000;

	on_pageflip(output);

	return 0;
}

static int
retire_fence_early_cb(int fd, uint32_t mask, void *data)
{
	struct drm_output *output = (struct drm_output *) data;
	struct early_layer *early_layer, *next_early_layer;
	static bool first_fence = true;
	struct timespec ts;

	if (first_fence) {
		first_fence = false;
		bootkpi_log_init();
		bootkpi_log_line("W - first early frame have been displayed");
	}

	wl_event_source_remove(output->retire_fence_source);
	output->retire_fence_source = NULL;

   if (get_fence_timestamp(output->retire_fence_fd, &ts))
		weston_compositor_read_presentation_clock(output->base.compositor, &ts);

	close(output->retire_fence_fd);
	output->retire_fence_fd = -1;
	output->last_vblank.sec = ts.tv_sec;
	output->last_vblank.usec = ts.tv_nsec / 1000;

	wl_list_for_each_safe(early_layer, next_early_layer, &output->commited_early_list, link)
		destroy_early_layer(early_layer);

	assert(wl_list_empty(&output->commited_early_list));
	wl_list_insert_list(&output->commited_early_list, &output->early_layer_list);
	wl_list_init(&output->early_layer_list);

	weston_output_finish_frame(&output->base, &ts, 0);

	return 0;
}

static int
drm_backend_create_gl_renderer(struct drm_backend *b);

static int
drm_output_init_egl(struct drm_output *output, struct drm_backend *b);

static void
drm_set_dpms(struct weston_output *output_base, enum dpms_enum level);

static int
drm_output_init_pixman(struct drm_output *output, struct drm_backend *b);

static void
pll_destroy(struct wl_client *client,
	struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
pll_enable_ppm(struct wl_client *client,
	struct wl_resource *pll, int32_t enable)
{
	struct weston_compositor *compositor = wl_resource_get_user_data(pll);
	struct weston_output *output, *next;

	wl_list_for_each_safe(output, next, &compositor->output_list, link) {
		if (output->enable_ppm)
			output->enable_ppm(output, enable);
	}
}

static void
pll_set_ppm(struct wl_client *client,
		    struct wl_resource *pll,
		    int32_t ppm)
{
	struct weston_compositor *compositor = wl_resource_get_user_data(pll);
	struct weston_output *output, *next;

	wl_list_for_each_safe(output, next, &compositor->output_list, link) {
		if (output->set_ppm)
			output->set_ppm(output, ppm);
	}
}

static const struct wl_pll_interface pll_interface = {
	pll_destroy,
	pll_enable_ppm,
	pll_set_ppm
};

static void
bind_pll(struct wl_client *client,
	    void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &wl_pll_interface, 1, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &pll_interface,
		                       data, NULL);
}
static int
finish_init(void *data)
{
	struct drm_backend *b = data;
	struct drm_output *output;
	int fd;
	bool handoff = false;
	int ret = 0;

	if (b->use_pixman) {
		if (pixman_renderer_init(b->compositor) < 0) {
			weston_log("Failed to create pixman renderer.\n");
			return -1;
		}

		wl_list_for_each(output, &b->compositor->output_list, base.link)
			drm_output_init_pixman(output, b);
	} else {
		/* destroy early renderer if it exsit */
		b->compositor->renderer->destroy(b->compositor);
		if (drm_backend_create_gl_renderer(b) < 0) {
			weston_log("Failed to create GL renderer.\n");
			return -1;
		}

		wl_list_for_each(output, &b->compositor->output_list, base.link) {
			drm_output_init_egl(output, b);
			drm_set_dpms(&output->base, WESTON_DPMS_ON);
		}

		if (b->compositor->renderer->import_dmabuf) {
			if (linux_dmabuf_setup(b->compositor) < 0)
				weston_log("Error: initializing dmabuf "
						"support failed.\n");
		}
		if (screen_capture_c_interface.setup(b->compositor) < 0)
				weston_log("Error: initializing screen_capture_setup "
						"support failed.\n");

	}
	if (!wl_global_create(b->compositor->wl_display, &wl_pll_interface, 1,
			    		b->compositor, bind_pll))
		weston_log("Error: initializing wl_pll_interface failed.\n");

	/* Load touch_only_surface module at runtime and register the global object */
	ret = load_touch_only_surface_module();
	if (ret < 0) {
		weston_log("Error: loading touch_only_surface module failed.\n");
	} else {
		ret = touch_only_surface_init_func(b->compositor);
		if (ret < 0)
			weston_log("Error: initializing touch_only_surface failed.\n");
	}

	if (!b->early_boot)
		goto out;

	wl_list_for_each(output, &b->compositor->output_list, base.link) {
		struct early_layer *early_layer, *next_early_layer;

		if (!wl_list_empty(&output->early_layer_list) || !wl_list_empty(&output->commited_early_list))
			handoff = true;

		/*
		* Schedule a repaint for every output to make sure
		* outputs display consistently after switching.
		*/
		weston_output_schedule_repaint(&output->base);
	}

	/*
	 * attach early pipes to sdm display to make sure the early pipes could
	 * be using correctly by sdm, current frame is using the pipes
	 */
	if (handoff) {
		ret = sdm_service->SetPlaneInitState();
		if (ret)
			weston_log("handoff early pipes error %d\n", ret);
	}

	wl_event_source_remove(b->finish_full_init);
	early_drm_display_deinit(false, b->drm.fd);
out:
	b->sdm_repaint = true;
	weston_log("full initialization finished, switched to sdm repaint!\n");
	bootkpi_log_init();
	bootkpi_log_line("W - backend full ready");
	return 0;
}

static int
drm_output_repaint_early(struct weston_output *output_base)
{
	struct drm_output *output = (struct drm_output *) output_base;
	struct early_layer *early_layer, *next_early_layer;
	static bool first_commit = true;
	struct drm_backend *backend = to_drm_backend(output_base->compositor);
	int ret = -1;

	if (output->prev_layer_none_commit && output->layer_none_commit)
		weston_log("skip commit if two consecutive frames have no layers\n");
	else {
		ret = early_commit(output);
		if (first_commit) {
			first_commit = false;
			bootkpi_log_init();
			bootkpi_log_line("W - first early commit submitted");
		}
	}

	if (ret) {
		wl_list_for_each_safe(early_layer, next_early_layer, &output->commited_early_list, link)
			destroy_early_layer(early_layer);

		assert(wl_list_empty(&output->commited_early_list));
		wl_list_insert_list(&output->commited_early_list, &output->early_layer_list);
		wl_list_init(&output->early_layer_list);
		wl_event_source_timer_update(output->finish_frame_timer, 16);
	} else {
		if (output->retire_fence_fd > 0) {
			struct wl_event_loop *loop =
				wl_display_get_event_loop(output->base.compositor->wl_display);

			output->retire_fence_source = wl_event_loop_add_fd(loop,
				output->retire_fence_fd, WL_EVENT_READABLE,
				retire_fence_early_cb, output);
		}
	}

	/*
	* In order not to make influence on boot KPI, full_init_thread
	* does not start to do initiliaztion until the first repaint finished.
	*/
	if (backend->first_repaint) {
		backend->first_repaint = false;
		pthread_mutex_lock(&full_init_mutex);
		pthread_cond_signal(&full_init_cond);
		pthread_mutex_unlock(&full_init_mutex);
	}
	return 0;
}

static void
post_repaint(struct drm_output *output, bool is_virtual_output)
{
		struct sdm_layer *sdm_layer, *next_sdm_layer;

		if (!is_virtual_output) {
			drm_output_release_fb(output, output->current);
		}
		output->current = output->next;
		output->next = NULL;
		output->frame_pending = 0;

		wl_list_for_each_safe(sdm_layer, next_sdm_layer, &output->commited_layer_list, link) {
			destroy_sdm_layer(sdm_layer);
		}

		assert(wl_list_empty(&output->commited_layer_list));
		wl_list_insert_list(&output->commited_layer_list, &output->sdm_layer_list);
		wl_list_init(&output->sdm_layer_list);
		if (!is_virtual_output)
			wl_event_source_timer_update(output->finish_frame_timer, 1);
}

static int
output_repaint(struct weston_output *output_base,
		pixman_region32_t *damage, bool is_virtual_output)
{
	struct drm_output *output = (struct drm_output *) output_base;
	struct drm_backend *backend = to_drm_backend(output_base->compositor);
	struct drm_mode *mode;
	static bool commit = false;
	int ret = -1;
	struct sdm_layer *sdm_layer, *next_sdm_layer;

	if (output->destroy_pending || output->disable_pending)
		return -1;

	if (output_base->disable_planes)
		output_base->need_gpu_composition = true;

	if (!is_virtual_output && !output->next && output_base->need_gpu_composition) {
		drm_output_render(output, damage);
	}
	assert(wl_list_empty(&output->plane_flip_list));

	sdm_service->SetVSyncState(output->display_id, ENABLE, output);
	if (output->prev_layer_none_commit && output->layer_none_commit)
		weston_log("skip commit if two consecutive frames have no layers\n");
	else if (output->layer_none_commit){
		ret = sdm_service->Flush(output->display_id);
	} else {
		ret = sdm_service->Commit(output->display_id, output);

		if (!commit) {
			commit = true;
			bootkpi_log_init();
			bootkpi_log_line("W - first commit submitted");
		}
	}

	if (ret) {
		weston_log("fail to commit or flush to sdm display! err=%d\n", ret);
		post_repaint(output, is_virtual_output);
	} else {
		output->frame_pending = 1;

#if ENABLE_PAGEFLIP
		if (output->retire_fence_fd >= 0)
			close(output->retire_fence_fd);
#else
		if (output->retire_fence_fd >= 0) {
			struct wl_event_loop *loop =
					wl_display_get_event_loop(output->base.compositor->wl_display);

			output->retire_fence_source = wl_event_loop_add_fd(loop,
					output->retire_fence_fd, WL_EVENT_READABLE,
					retire_fence_cb, output);
		} else {
			/* if Commit doesn't return fail, but retire fence fd is not valid, it means
			 * this Commit was succed to HW, but creating fence failed in this
			 * Commit. In this case, call post_repaint to update timer for next
			 * frame
			 */
			weston_log("get fence fail, maybe some error happen\n");
			post_repaint(output, is_virtual_output);
		}
#endif
	}
	return 0;
}

static void
do_screen_capture(struct screen_capture *screen_cap,
		pixman_region32_t *damage)
{
	struct screen_capture_buffer *cap_buf = screen_cap->next;

	/*
	 * Decrease the attached refcnt after increasing composition refcnt to
	 * avoid releasing buffer in advance
	 */
	weston_buffer_reference(&screen_cap->buf_ref, cap_buf->buffer, BUFFER_MAY_BE_ACCESSED);
	weston_buffer_reference(&cap_buf->buf_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);

	if (screen_cap->fallback_gpu) {
		screen_cap->compositor->renderer->capture_screen(screen_cap->mirror_output,
						cap_buf->buffer, damage);

		/* Release the buffer once GPU composition is completed */
		weston_buffer_reference(&screen_cap->buf_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);

		free(cap_buf);
		screen_cap->next = NULL;
		screen_cap->view = NULL;
	} else {
		output_repaint(screen_cap->virtual_output, damage, true);
	}
}

static int
drm_output_repaint(struct weston_output *output_base)
{
	struct drm_backend *backend = to_drm_backend(output_base->compositor);
	struct screen_capture *screen_cap = backend->screen_cap;
	pixman_region32_t damage;

	pixman_region32_init(&damage);

	weston_output_flush_damage_for_primary_plane(output_base, &damage);

	/* Backend is not full ready, do early repaint. */
	if (!backend->sdm_repaint) {
		pixman_region32_fini(&damage);
		return drm_output_repaint_early(output_base);
	}

	output_repaint(output_base, &damage, false);

	/* Do output repaint for virtual output. */
	if (screen_capture_c_interface.is_capture_ready(screen_cap, output_base) && screen_cap->next)
		do_screen_capture(screen_cap, &damage);

	pixman_region32_fini(&damage);

	return 0;
}

static int
drm_output_start_repaint_loop(struct weston_output *output_base)
{
	struct drm_output *output = (struct drm_output *) output_base;
	struct drm_backend *backend = to_drm_backend(output_base->compositor);
	uint32_t fb_id;
	struct timespec ts, tnow;
	struct timespec vbl2now;
	int64_t refresh_nsec;
	int ret;
	drmVBlank vbl = {
		.request.type = DRM_VBLANK_RELATIVE,
		.request.sequence = 0,
		.request.signal = 0,
	};

	if (output->destroy_pending || output->disable_pending)
		return 0;

	if (!output->current) {
		/* We can't page flip if there's no mode set */
		goto finish_frame;
	}

	// SDM backend cannot inovke page-flip as it needs to construct
	// layer stack from drm_assign_planes. Since we cannot push the frame
	// handle this funciton gracefully
	goto finish_frame;

	/* TODO (user): To get time stamp information from SDM interface */
	/* Try to get current msc and timestamp via instant query */

finish_frame:
	/* if we cannot page-flip, immediately finish frame */
	weston_output_finish_frame(output_base, NULL,
			WP_PRESENTATION_FEEDBACK_INVALID);
	return 0;
}

/**
 * Begin a new repaint cycle
 *
 * Called by the core compositor at the beginning of a repaint cycle. Creates
 * a new pending_state structure to own any output state created by individual
 * output repaint functions until the repaint is flushed or cancelled.
 */
static void *
drm_repaint_begin(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);

	if (weston_log_scope_is_enabled(b->debug)) {
		char *dbg = weston_compositor_print_scene_graph(b->compositor);
		drm_debug(b, "[repaint] Beginning repaint\n");
		drm_debug(b, "%s", dbg);
		free(dbg);
	}

	return NULL;
}

/**
 * Flush a repaint set
 *
 * Called by the core compositor when a repaint cycle has been completed
 * and should be flushed. Frees the pending state, transitioning ownership
 * of the output state from the pending state, to the update itself. When
 * the update completes (see drm_output_update_complete), the output
 * state will be freed.
 */
static int
drm_repaint_flush(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);

	drm_debug(b, "[repaint] flushed\n");

	return 0;
}

/**
 * Cancel a repaint set
 *
 * Called by the core compositor when a repaint has finished, so the data
 * held across the repaint cycle should be discarded.
 */
static void
drm_repaint_cancel(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);

	drm_debug(b, "[repaint] cancel\n");
}

static void
drm_output_update_msc(struct drm_output *output, unsigned int seq)
{
	uint64_t msc_hi = output->base.msc >> 32;

	if (seq < (output->base.msc & 0xffffffff))
		msc_hi++;

	output->base.msc = (msc_hi << 32) + seq;
}

#if ENABLE_PAGEFLIP
static void
pageflip_handler(unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
	struct drm_output *output = (struct drm_output *) data;
	uint64_t v = 1;

	output->last_vblank.frame = frame;
	output->last_vblank.sec = sec;
	output->last_vblank.usec = usec;

	write(output->pageflip_ev_fd, &v, sizeof v);
}
#else
static void
pageflip_handler(unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
}
#endif

static int
drm_output_enable_pageflip(struct drm_output *output)
{
	struct wl_event_loop *loop;

	output->pageflip_ev_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (output->pageflip_ev_fd < 0){
		return -1;
	}

	loop = wl_display_get_event_loop(output->base.compositor->wl_display);

	output->pageflip_ev_source = wl_event_loop_add_fd(loop, output->pageflip_ev_fd,
		WL_EVENT_READABLE, on_pageflip_vsync, output);

	return 0;
}

static void
drm_output_disable_pageflip(struct drm_output *output)
{
	if (output->pageflip_ev_source != NULL) {
		wl_event_source_remove(output->pageflip_ev_source);
		output->pageflip_ev_source = NULL;
	}

	if (output->pageflip_ev_fd != -1) {
		close(output->pageflip_ev_fd);
		output->pageflip_ev_fd = -1;
	}
}

static void
drm_output_deinit(struct weston_output *base);

static void
drm_output_destroy(struct weston_output *output_base);

/**
 * process pageflip triggered by fence
 */
static void
on_pageflip(struct drm_output *output)
{
	struct timespec ts;
	uint32_t flags = WP_PRESENTATION_FEEDBACK_KIND_VSYNC |
			 WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION |
			 WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION;
		struct sdm_layer *sdm_layer, *next_sdm_layer;

	drm_output_update_msc(output, output->last_vblank.frame++);

	/*
	* After switching to sdm repaint, need to destroy
	* committed early layers if there is any
	*/
	if (!wl_list_empty(&output->commited_early_list)) {
		struct early_layer *early_layer, *next_early_layer;

		wl_list_for_each_safe(early_layer, next_early_layer, &output->commited_early_list, link) {
			destroy_early_layer(early_layer);
		}
	}

	/* We don't set page_flip_pending on start_repaint_loop, in that case
	* we just want to page flip to the current buffer to get an accurate
	* timestamp */
	if (output->frame_pending) {
		drm_output_release_fb(output, output->current);
		output->current = output->next;
		output->next = NULL;
		output->frame_pending = 0;

		wl_list_for_each_safe(sdm_layer, next_sdm_layer, &output->commited_layer_list, link) {
			destroy_sdm_layer(sdm_layer);
		}

		assert(wl_list_empty(&output->commited_layer_list));
		wl_list_insert_list(&output->commited_layer_list, &output->sdm_layer_list);
		wl_list_init(&output->sdm_layer_list);
	}

	if (output->destroy_pending) {
		output->disable_pending = 0;
		output->destroy_pending = 0;
		drm_output_destroy(&output->base);
	} else if (output->disable_pending) {
		output->disable_pending = 0;
		weston_output_disable(&output->base);
	} else if (!output->frame_pending) {
		ts.tv_sec = output->last_vblank.sec;
		ts.tv_nsec = output->last_vblank.usec * 1000;

		weston_output_finish_frame(&output->base, &ts, flags);

		/* We can't call this from frame_notify, because the output's
		* repaint needed flag is cleared just after that */
		if (output->recorder)
		weston_output_schedule_repaint(&output->base);
	}
}

/**
 * process pageflip triggered by vblank
 */
static int
on_pageflip_vsync(int fd, uint32_t mask, void *data)
{
	struct timespec ts;
	struct drm_output *output = (struct drm_output *) data;
	uint32_t flags = WP_PRESENTATION_FEEDBACK_KIND_VSYNC |
			 WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION |
			 WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION;
	struct sdm_layer *sdm_layer, *next_sdm_layer;
	uint64_t v = 1;
	static bool first_vsync = true;

	if (first_vsync) {
		first_vsync = false;
		bootkpi_log_init();
		bootkpi_log_line("W - first frame have been displayed");
	}

	read(output->pageflip_ev_fd, &v, sizeof v);

	drm_output_update_msc(output, output->last_vblank.frame++);

	/*
	 * After switching to sdm repaint, need to destroy
	 * committed early layers if there is any
	 */
	if (!wl_list_empty(&output->commited_early_list)) {
		struct early_layer *early_layer, *next_early_layer;

		wl_list_for_each_safe(early_layer, next_early_layer, &output->commited_early_list, link) {
			destroy_early_layer(early_layer);
		}
	}

	/* We don't set page_flip_pending on start_repaint_loop, in that case
	 * we just want to page flip to the current buffer to get an accurate
	 * timestamp */
	if (output->frame_pending) {
		drm_output_release_fb(output, output->current);
		output->current = output->next;
		output->next = NULL;
		output->frame_pending = 0;

		wl_list_for_each_safe(sdm_layer, next_sdm_layer, &output->commited_layer_list, link) {
			destroy_sdm_layer(sdm_layer);
		}

		assert(wl_list_empty(&output->commited_layer_list));
		wl_list_insert_list(&output->commited_layer_list, &output->sdm_layer_list);
		wl_list_init(&output->sdm_layer_list);
	}

	if (output->destroy_pending) {
		output->disable_pending = 0;
		output->destroy_pending = 0;
		drm_output_destroy(&output->base);
	} else if (output->disable_pending) {
		output->disable_pending = 0;
		weston_output_disable(&output->base);
	} else if (!output->frame_pending) {
		ts.tv_sec = output->last_vblank.sec;
		ts.tv_nsec = output->last_vblank.usec * 1000;

		/* Cancel fallback timer to avoid racing with weston_output_repaint_failed. */
		wl_event_source_timer_update(output->finish_frame_timer, 0);

		weston_output_finish_frame(&output->base, &ts, flags);

		/* We can't call this from frame_notify, because the output's
		 * repaint needed flag is cleared just after that */
		if (output->recorder)
			weston_output_schedule_repaint(&output->base);
	}
	return 0;
}

static int
drm_view_transform_supported(struct weston_view *ev)
{
	return !ev->transform.enabled ||
		(ev->transform.matrix.type < WESTON_MATRIX_TRANSFORM_ROTATE);
}

static void
destroy_sdm_layer(struct sdm_layer *layer)
{
	pixman_region32_fini(&layer->overlap);

	/* SHM buffer could already been released after gpu composition.
	 * For GPU composition buffers, let renderer take charge to manage buffer object reference
	 * to prevent unexpected buffer release event.
	 */
	if (!layer->is_skip) {
		weston_buffer_reference(&layer->buffer_ref, NULL, BUFFER_WILL_NOT_BE_ACCESSED);
	} else {
		layer->buffer_ref.buffer = NULL;
	}

	wl_list_remove(&layer->link);
	if (layer->bo) {
		gbm_bo_destroy(layer->bo);
	}
	free(layer);
}

static struct sdm_layer *
create_sdm_layer(struct drm_output *output, struct weston_paint_node *pnode, pixman_region32_t *overlap,
		 bool is_cursor, bool is_skip)
{
	struct sdm_layer *layer;
	struct weston_view *ev = pnode->view;

	layer = zalloc(sizeof(*layer));
	if (layer == NULL) {
		return NULL;
	}

	layer->view = ev;
	layer->pnode = pnode;
	layer->is_cursor = is_cursor;
	layer->is_skip = is_skip;

	pixman_region32_init(&layer->overlap);
	pixman_region32_copy(&layer->overlap, overlap);

	/* SHM buffer could already been released after gpu composition.
	 * For GPU composition buffers, let renderer take charge to manage buffer object reference
	 * to prevent unexpected buffer release event.
	 */
	if (!is_skip) {
		weston_buffer_reference(&layer->buffer_ref, ev->surface->buffer_ref.buffer, BUFFER_MAY_BE_ACCESSED);
	} else {
		layer->buffer_ref.buffer = ev->surface->buffer_ref.buffer;
	}

	return layer;
}

static bool
is_skip_view(struct weston_view *ev, struct drm_output *output)
{
	struct weston_surface *es = ev->surface;
	struct weston_buffer_viewport *viewport = &ev->surface->buffer_viewport;
	bool skip = false;

	/* Don't support hw rotate now */
	if (viewport->buffer.transform != output->base.transform)
		return true;

	if (!drm_view_transform_supported(ev))
		return true;

	/* skip the view which is overlapped with multiple output */
	if (ev->output_mask != (1u << output->base.id))
		skip = true;

	/* skip animation fade-out view */
	if (!weston_view_has_valid_buffer(ev)) {
		skip = true;
	}  else if (wl_shm_buffer_get(es->buffer_ref.buffer->resource)) {
		skip = true;
	}

	return skip;
}

static bool
is_completely_covered_view(struct weston_view *ev, pixman_region32_t *above_opaque)
{
	pixman_region32_t temp;

	pixman_region32_init(&temp);
	pixman_region32_copy(&temp, &ev->transform.boundingbox);
	pixman_region32_subtract(&temp, &temp, above_opaque);

	if (pixman_region32_not_empty(&temp))
		ev->is_completely_covered = false;
	else
		ev->is_completely_covered = true;
	pixman_region32_fini(&temp);

	return ev->is_completely_covered;
}

static void
drm_assign_planes_early(struct weston_output *output_base)
{
	struct drm_output *output = to_drm_output(output_base);
	struct weston_paint_node *pnode, *next;
	struct early_layer *early_layer;

	assert(wl_list_empty(&output->early_layer_list));
	output->view_count = 0;

	wl_list_for_each_safe(pnode, next, &output->base.paint_node_z_order_list, z_order_link) {
		struct weston_view *ev = pnode->view;
		bool is_skip = false;
		struct weston_surface *es = ev->surface;

		/*
		* Only buffers with flag GBM_BUFFER_PARAMS_FLAGS_EARLY_DISPLAY
		* are displayed during early light-weight stage, so keep other buffers
		* until it switches to sdm repaint to make sure they are not lost.
		*/
		es->keep_buffer = true;

		if (!output->early_display_enable)
			continue;

		is_skip = is_skip_view(ev, output);
		if (is_skip)
			continue;

		if (es->buffer_ref.buffer) {
			struct gbm_buffer *gbm_buf =
					gbm_buffer_backend_c_interface.buffer_get(es->buffer_ref.buffer->resource);

			if (gbm_buf && gbm_buf->flags & GBM_BUFFER_PARAMS_FLAGS_EARLY_DISPLAY) {
				early_layer = create_early_layer(pnode);
				if (!early_layer)
					continue;
				wl_list_insert(output->early_layer_list.prev, &early_layer->link);

				if (early_layer_prepare(early_layer, output)) {
					destroy_early_layer(early_layer);
					continue;
				}
				output->view_count++;
			}
		}
	}

	output->prev_layer_none_commit = output->layer_none_commit;
	if (output->view_count) {
		if (early_prepare(output)) {
			struct early_layer *next_early_layer;

			output->layer_none_commit = true;
			wl_list_for_each_safe(early_layer, next_early_layer, &output->early_layer_list, link)
				destroy_early_layer(early_layer);
		} else
			output->layer_none_commit = false;
	} else
		output->layer_none_commit = true;

	return;
}

static void
prepare_virtual_output(struct drm_output *virtual_output, struct weston_output *output_base)
{
	virtual_output->base = *output_base;

	/* clear the list which is established by the original output */
	wl_list_init(&virtual_output->sdm_layer_list);
}

static int
assign_planes(struct weston_output *output_base, bool is_virtual_output)
{
	struct drm_backend *b = to_drm_backend(output_base->compositor);
	struct drm_output *output = (struct drm_output *)output_base;
	struct weston_view *ev;
	struct weston_paint_node *pnode, *next;
	pixman_region32_t above_opaque, surface_opaque;
	struct weston_plane *primary, *next_plane;
	struct sdm_layer *sdm_layer, *next_sdm_layer;
	output->view_count = 0;
	bool has_GPU_composition = false;

	assert(wl_list_empty(&output->sdm_layer_list));

	/*
	 * Find a surface for each sprite in the output using some heuristics:
	 * 1) size
	 * 2) frequency of update
	 * 3) opacity (though some hw might support alpha blending)
	 * 4) clipping (this can be fixed with color keys)
	 *
	 * The idea is to save on blitting since this should save power.
	 * If we can get a large video surface on the sprite for example,
	 * the main display surface may not need to update at all, and
	 * the client buffer can be used directly for the sprite surface
	 * as we do for flipping full screen surfaces.
	 */
	pixman_region32_init(&above_opaque);
	primary = &output_base->primary_plane;

	/* 1. Compute how many views which can be handled by SDM module */
	/* Some views may neither be composited by GPU nor display engine directly,
	 * they are in the "skip" status, even no buffer is attached. We can't pass them
	 * to SDM because format check will fail which may cause SDM can't filter
	 * correct strategy result. If so, assign those views directly to primary plane.
	 */
	wl_list_for_each_safe(pnode, next, &output->base.paint_node_z_order_list, z_order_link) {
		struct weston_view *ev = pnode->view;
		bool is_cursor = false;
		bool is_skip = false;
		struct weston_surface *es = ev->surface;
		struct gbm_buffer *gbm_buf = NULL;

		drm_debug(b, "\t\t\t[view] evaluating view %p for output %s (%lu)\n",
			ev, output->base.name,
			(unsigned long) output->base.id);

		/* Test whether this buffer can ever go into a plane:
		 * non-shm, or small enough to be a cursor.
		 *
		 * Also, keep a reference when using the pixman renderer.
		 * That makes it possible to do a seamless switch to the GL
		 * renderer and since the pixman renderer keeps a reference
		 * to the buffer anyway, there is no side effects.
		 */
		if (!is_virtual_output) {
			if (b->use_pixman ||
				(es->buffer_ref.buffer &&
				(!wl_shm_buffer_get(es->buffer_ref.buffer->resource) ||
				(ev->surface->width <= 64 && ev->surface->height <= 64))))
				es->keep_buffer = true;
			else
				es->keep_buffer = false;
		}

		/* Skip the screen capture view as it's not used for display */
		if (screen_capture_c_interface.is_screen_capture_view(ev)) {
			/* surface of screen capture don't need to keep its buffer, it
			 * keeps in screencapture attached_buf_list. screen_capture_attach
			 * increase the buffer refcount and do_screen_capture decrease the
			 * buffer refcount.
			 */
			es->keep_buffer = false;
			continue;
		}


		/* Skip view that doesn't belong to the output, no need to increase overhead for SDM */
		if (!(ev->output_mask & (1u << output->base.id))) {
			drm_debug(b, "\t\t\t\t[view] ignoring view %p (not on our output)\n", ev);
			if (es->keep_buffer == false)
				weston_paint_node_move_to_plane(pnode, primary);
			continue;
		}

		if (is_completely_covered_view(ev, &above_opaque)) {
			drm_debug(b, "\t\t\t\t[view] ignoring view %p (occluded on our output)\n", ev);

			if (es->keep_buffer == false)
				weston_paint_node_move_to_plane(pnode, primary);
			else
				pnode->plane = NULL;
			continue;
		}

		is_skip = is_skip_view(ev, output);

		sdm_layer = create_sdm_layer(output, pnode, &above_opaque, is_cursor, is_skip);
		if (sdm_layer == NULL) {
			weston_log("Create sdm layer failed.\n");
			goto err_out;
		}

		wl_list_insert(output->sdm_layer_list.prev, &sdm_layer->link);

		output->view_count++;
		pixman_region32_union(&above_opaque, &above_opaque, &ev->transform.opaque);
		/* if it's yuv buffer and alpha is 1.0, its whole boundingbox is the opaque region*/
		if ((es->buffer_ref.buffer) && 
			gbm_buffer_backend_c_interface.is_yuv_buffer(es->buffer_ref.buffer) && 
			ev->alpha == 1.0)
			pixman_region32_union(&above_opaque, &above_opaque, &ev->transform.boundingbox);
	}

	/*
	 * SDM always need FB target layer, however, in Weston there is no explicit
	 * fb target view, need to fake one
	 */
	output->view_count++;
	/*
	 * repaint virtual output by gpu now, no need to do Prepare here
	 * all sdm layers' composition_type keep default value SDM_COMPOSITION_GPU
	 */
	if (!is_virtual_output)
		sdm_service->Prepare(output->display_id, output);
	wl_list_for_each_safe(sdm_layer, next_sdm_layer, &output->sdm_layer_list, link) {
		next_plane = primary;
		ev = sdm_layer->view;
		/* Move to primary plane if Strategy set it to GPU composition */
		if (sdm_layer->composition_type == SDM_COMPOSITION_GPU) {
			if (!is_virtual_output) {
				drm_debug(b, "\t\t\t\t[view] view %p will be placed on the renderer[primary plane]\n", ev);
				weston_paint_node_move_to_plane(sdm_layer->pnode, next_plane);
				ev->psf_flags = 0;
			}

			destroy_sdm_layer(sdm_layer);
			has_GPU_composition = true;
		} else {
			if (!is_virtual_output) {
				drm_debug(b, "\t\t\t\t[view] view %p will be placed on the overlay plane\n", ev);
				/* Composed by Display Hardware directly */
				ev->psf_flags = WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY;
				/* Set the view's plane back to NULL so that it is not composed by GPU */
				sdm_layer->pnode->plane = NULL;
			}
		}
	}

err_out:
	/*
	 * Display WB2 composition requires all views must be overlay. Remove all views once
	 * any view goes through GPU composition.
	 */
	if (is_virtual_output && has_GPU_composition) {
		/* fallback to gpu composition if no layer */
		wl_list_for_each_safe(sdm_layer, next_sdm_layer, &output->sdm_layer_list, link) {
			destroy_sdm_layer(sdm_layer);
		}
	}

	pixman_region32_fini(&above_opaque);

	return has_GPU_composition;
}

static void
drm_assign_planes(struct weston_output *output_base)
{
	struct drm_backend *b = to_drm_backend(output_base->compositor);
	struct screen_capture *screen_cap = b->screen_cap;
	bool has_GPU_composition = false;

	drm_debug(b, "\t[repaint] preparing state for output %s (%lu)\n",
		  output_base->name, (unsigned long) output_base->id);

	/* If backend is not full ready, do early assign planes */
	if (!b->sdm_repaint) {
		drm_assign_planes_early(output_base);
		return;
	}

	/* Do assign planes for normal output */
	has_GPU_composition = assign_planes(output_base, false);
	output_base->need_gpu_composition = has_GPU_composition;

	/*
	 * Do assign planes for virtual output. If GPU composition already happens,
	 * no need to check if display WB2 composition can work again as the HW pipe
	 * resource is already stressed.
	 * If the last attached buffer has not been consumed yet, skip this commit
	 * until it's consumed to guarantee each client buffer has content update.
	 */
	if (screen_capture_c_interface.is_capture_ready(screen_cap, output_base) &&
		!wl_list_empty(&screen_cap->attached_buf_list) &&
		!screen_cap->next) {
		/* Pick the first entry in the attached list */
		screen_cap->next = container_of(screen_cap->attached_buf_list.next,
						struct screen_capture_buffer, link);
		wl_list_remove(&screen_cap->next->link);

		if (!has_GPU_composition) {
			struct screen_capture_buffer *cap_buf = NULL;
			struct drm_output *virtual_output = (struct drm_output *)screen_cap->virtual_output;

			/* Some settings of mirror output may be changed here, need to update them
			 * to virtual output as they will be used by SDM and GPU composition. */
			prepare_virtual_output(virtual_output, output_base);

			/* TODO: Update output buffer */
			screen_cap->fallback_gpu = assign_planes(virtual_output, true);
		} else {
			screen_cap->fallback_gpu = true;
		}
	}

	return;
}

static void
drm_output_fini_pixman(struct drm_output *output);

/**
 * Destroys a mode, and removes it from the list.
 */
static void
drm_output_destroy_mode(struct drm_backend *backend, struct drm_mode *mode)
{
	wl_list_remove(&mode->base.link);
	free(mode);
}

/** Destroy a list of drm_modes
 *
 * @param backend The backend for releasing mode property blobs.
 * @param mode_list The list linked by drm_mode::base.link.
 */
static void
drm_mode_list_destroy(struct drm_backend *backend, struct wl_list *mode_list)
{
	struct drm_mode *mode, *next;

	wl_list_for_each_safe(mode, next, mode_list, base.link)
		drm_output_destroy_mode(backend, mode);
}

static void
drm_output_destroy(struct weston_output *output_base)
{
	struct drm_output *output = (struct drm_output *) output_base;
	struct drm_backend *b = to_drm_backend(output_base->compositor);
	struct sdm_layer *sdm_layer, *next_sdm_layer;

	if (output->frame_pending) {
		if (!output_base->compositor->shutting_down) {
			/* We are not shutting down, so we can wait for flip
			 * completion. */
			output->destroy_pending = 1;
			weston_log("delaying output destruction because of a " \
				   "pending flip, wait until it completes\n");
			return;
		} else {
			weston_log("destroying output %s (id %u) with a pending " \
				   "flip, but as we are shutting down we can't " \
				   "wait to destroy it when the flip completes... " \
				   "destroying it now\n", output_base->name, output_base->id);
		}
	}

	if (output->finish_frame_timer)
		wl_event_source_remove(output->finish_frame_timer);

	if (output->backlight)
		backlight_destroy(output->backlight);

	if (output->base.enabled)
		drm_output_deinit(&output->base);

	wl_list_for_each_safe(sdm_layer, next_sdm_layer, &output->commited_layer_list, link)
		destroy_sdm_layer(sdm_layer);

	drm_mode_list_destroy(b, &output->base.mode_list);

#if ENABLE_PAGEFLIP
	drm_output_disable_pageflip(output);
#endif

	weston_output_release(&output->base);

	free(output);
}

/**
 * Find the closest-matching mode for a given target
 *
 * Given a target mode, find the most suitable mode amongst the output's
 * current mode list to use, preferring the current mode if possible, to
 * avoid an expensive mode switch.
 *
 * @param output DRM output
 * @param target_mode Mode to attempt to match
 * @returns Pointer to a mode from the output's mode list
 */
static struct drm_mode *
choose_mode (struct drm_output *output, struct weston_mode *target_mode)
{
	//TODO(user): will be implemented in SDM backend
	struct drm_mode *tmp_mode = NULL;

	return tmp_mode;
}

static int
drm_output_init_egl(struct drm_output *output, struct drm_backend *b);

static int
drm_output_init_pixman(struct drm_output *output, struct drm_backend *b);

static int
drm_output_switch_mode(struct weston_output *output_base, struct weston_mode *mode)
{
	struct drm_output *output;
	struct drm_mode *drm_mode;
	struct drm_backend *b;

	if (output_base == NULL) {
		weston_log("output is NULL.\n");
		return -1;
	}

	if (mode == NULL) {
		weston_log("mode is NULL.\n");
		return -1;
	}

	b = to_drm_backend(output_base->compositor);
	output = (struct drm_output *)output_base;
	drm_mode  = choose_mode (output, mode);

	if (!drm_mode) {
		weston_log("%s, invalid resolution:%dx%d\n", __func__, mode->width, mode->height);
		return -1;
	}

	if (&drm_mode->base == output->base.current_mode)
		return 0;

	output->base.current_mode->flags = 0;

	output->base.current_mode = &drm_mode->base;
	output->base.current_mode->flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;

	/* reset rendering stuff. */
	drm_output_release_fb(output, output->current);
	drm_output_release_fb(output, output->next);
	output->current = output->next = NULL;

	if (b->use_pixman) {
		drm_output_fini_pixman(output);
		if (drm_output_init_pixman(output, b) < 0) {
			weston_log("failed to init output pixman state with "
					"new mode\n");
			return -1;
		}
	} else {
		gl_renderer->output_destroy(&output->base);
		gbm_surface_destroy(output->surface);

		if (drm_output_init_egl(output, b) < 0) {
			weston_log("failed to init output egl state with "
					"new mode");
			return -1;
		}
	}

	return 0;
}

static int
init_drm_early(struct drm_backend *b)
{
	b->drm.fd = early_get_drm_master();
	if (b->drm.fd < 0) {
		weston_log("failed to get drm master fd\n");
		return -1;
	}

	weston_log("init drm: drm fd = %d\n", b->drm.fd);

	b->base.supported_presentation_clocks = 1 << CLOCK_MONOTONIC;

	/* use render node to create gbm device */
	b->render_fd = drmOpen("msm_drm", NULL);
	if (b->render_fd < 0) {
		weston_log("failed to open drm render device (%d)\n", b->render_fd);
		return -1;
	}

	return 0;
}

static int
init_early_renderer(struct drm_backend *b)
{
	b->gbm = gbm_create_device(b->render_fd);
	if (!b->gbm) {
		weston_log("failed to create gbm device\n");
		return -1;
	}

	if (early_renderer_init(b->compositor, b->gbm) < 0) {
		gbm_device_destroy(b->gbm);
		return -1;
	}

	return 0;
}

static struct gbm_device *
create_gbm_device(int fd)
{
	struct gbm_device *gbm;

	gl_renderer = weston_load_module("gl-renderer.so",
					 "gl_renderer_interface", LIBWESTON_MODULEDIR);
	if (!gl_renderer)
		return NULL;

	/* GBM will load a dri driver, but even though they need symbols from
	 * libglapi, in some version of Mesa they are not linked to it. Since
	 * only the gl-renderer module links to it, the call above won't make
	 * these symbols globally available, and loading the DRI driver fails.
	 * Workaround this by dlopen()'ing libglapi with RTLD_GLOBAL. */
	dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL);

	gbm = gbm_create_device(fd);

	return gbm;
}

/* When initializing EGL, if the preferred buffer format isn't available
 * we may be able to susbstitute an ARGB format for an XRGB one.
 *
 * This returns 0 if substitution isn't possible, but 0 might be a
 * legitimate format for other EGL platforms, so the caller is
 * responsible for checking for 0 before calling gl_renderer->create().
 *
 * This works around https://bugs.freedesktop.org/show_bug.cgi?id=89689
 * but it's entirely possible we'll see this again on other implementations.
 */
static const struct pixel_format_info *
fallback_format_for(const struct pixel_format_info *format)
{
	return pixel_format_get_info_by_opaque_substitute(format->format);
}

static int
drm_backend_create_gl_renderer(struct drm_backend *b)
{
	const struct pixel_format_info *format[3] = {
		b->format,
		fallback_format_for(b->format),
		NULL,
	};
	struct gl_renderer_display_options options = {
		.egl_platform = EGL_PLATFORM_GBM_KHR,
		.egl_native_display = b->gbm,
		.egl_surface_type = EGL_WINDOW_BIT,
		.formats = format,
		.formats_count = 2,
	};

	if (format[1])
		options.formats_count = 3;

	return weston_compositor_init_renderer(b->compositor,
					WESTON_RENDERER_GL,
					&options.base);
}

/**
 * Add a mode to output's mode list
 *
 * Copy the supplied DRM mode into a Weston mode structure, and add it to the
 * output's mode list.
 *
 * @param output DRM output to add mode to
 * @param info DRM mode structure to add
 * @returns Newly-allocated Weston/DRM mode structure
 */
static struct drm_mode *
drm_output_add_mode(struct drm_output *output, const drmModeModeInfo *info)
{
	struct drm_mode *mode;
	uint64_t refresh;

	mode = malloc(sizeof *mode);
	if (mode == NULL)
		return NULL;

	mode->base.flags = WL_OUTPUT_MODE_CURRENT;
	mode->base.width = info->hdisplay;
	mode->base.height = info->vdisplay;

	/* Calculate higher precision (mHz) refresh rate */
	refresh = (info->clock * 1000000LL / info->htotal +
		   info->vtotal / 2) / info->vtotal;

	if (info->flags & DRM_MODE_FLAG_INTERLACE)
		refresh *= 2;
	if (info->flags & DRM_MODE_FLAG_DBLSCAN)
		refresh /= 2;
	if (info->vscan > 1)
		refresh /= info->vscan;

	mode->base.refresh = refresh;
	mode->mode_info = *info;

	if (info->type & DRM_MODE_TYPE_PREFERRED)
		mode->base.flags |= WL_OUTPUT_MODE_PREFERRED;

	wl_list_insert(output->base.mode_list.prev, &mode->base.link);

	return mode;
}

static int
drm_subpixel_to_wayland(int drm_value)
{
	switch (drm_value) {
	default:
	case DRM_MODE_SUBPIXEL_UNKNOWN:
		return WL_OUTPUT_SUBPIXEL_UNKNOWN;
	case DRM_MODE_SUBPIXEL_NONE:
		return WL_OUTPUT_SUBPIXEL_NONE;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB;
	case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR:
		return WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR;
	case DRM_MODE_SUBPIXEL_VERTICAL_RGB:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_RGB;
	case DRM_MODE_SUBPIXEL_VERTICAL_BGR:
		return WL_OUTPUT_SUBPIXEL_VERTICAL_BGR;
	}
}

/* returns a value between 0-255 range, where higher is brighter */
static uint32_t
drm_get_backlight(struct drm_output *output)
{
	long brightness, max_brightness, norm;

	brightness = backlight_get_brightness(output->backlight);
	max_brightness = backlight_get_max_brightness(output->backlight);

	/* convert it on a scale of 0 to 255 */
	norm = (brightness * 255)/(max_brightness);

	return (uint32_t) norm;
}

/* values accepted are between 0-255 range */
static void
drm_set_backlight(struct weston_output *output_base, uint32_t value)
{
	struct drm_output *output = (struct drm_output *) output_base;
	long max_brightness, new_brightness;

	if (!output->backlight)
		return;

	if (value > 255)
		return;

	max_brightness = backlight_get_max_brightness(output->backlight);

	/* get denormalized value */
	new_brightness = (value * max_brightness) / 255;

	backlight_set_brightness(output->backlight, new_brightness);
}

static drmModePropertyPtr
drm_get_prop(int fd, drmModeConnectorPtr connector, const char *name)
{
	drmModePropertyPtr props;
	int i;

	for (i = 0; i < connector->count_props; i++) {
		props = drmModeGetProperty(fd, connector->props[i]);
		if (!props)
			continue;

		if (!strcmp(props->name, name))
			return props;

		drmModeFreeProperty(props);
	}

	return NULL;
}

static void
drm_set_dpms(struct weston_output *output_base, enum dpms_enum level)
{
	struct drm_output *output = (struct drm_output *) output_base;

	if (!sdm_service) {
		weston_log("sdm service not ready, return\n");
		output->dpms = level;
		return;
	}

	weston_log("drm_set_dpms: Calling SDM to SetDisplaySatte.");
	int ret = sdm_service->SetDisplayState(output->display_id, level);

	if (ret) {
		weston_log("drm_set_dpms: Error! fail to set dpms level %d!", level);
		return;
	}

	output->dpms = level;
}

static void
drm_enable_ppm(struct weston_output *output_base, int32_t enable)
{
	struct drm_output *output = (struct drm_output *) output_base;
	int ret = 0;

	if (output->display_id < 0) {
		weston_log("invalid display id\n");
		return;
	}

	if (!sdm_service) {
		weston_log("sdm service not ready, drm_enable_ppm return\n");
		return;
	}

	ret = sdm_service->EnablePllUpdate(output->display_id, enable);
	if (ret) {
		weston_log("DRM: PLL: enable pll update failed for %d\n",
				output->display_id);
	}

	return;
}

static void
drm_set_ppm(struct weston_output *output_base, int32_t ppm)
{
	struct drm_output *output = (struct drm_output *) output_base;
	int ret = 0;

	if (output->display_id < 0) {
		weston_log("invalid display id\n");
		return;
	}

	if (!sdm_service) {
		weston_log("sdm service not ready, drm_set_ppm return\n");
		return;
	}

	ret = sdm_service->UpdateDisplayPll(output->display_id, ppm);
	if (ret) {
		weston_log("DRM: PLL: update display pll failed for %d\n",
				output->display_id);
	}

	return;
}

/* Init output state that depends on gl or gbm */
static int
drm_output_init_egl(struct drm_output *output, struct drm_backend *b)
{
	const struct pixel_format_info *format[2] = {
		output->format,
		fallback_format_for(output->format),
	};
	int i, flags;
	flags = GBM_BO_USE_SCANOUT |
			GBM_BO_USE_RENDERING |
			GBM_BO_USAGE_UBWC_ALIGNED_QTI |
			GBM_BO_USAGE_HW_RENDERING_QTI;

	if (property_get_bool("weston.protected_context", false)) {
		flags |= GBM_BO_USAGE_PROTECTED_QTI;
	}

	output->surface = gbm_surface_create(b->gbm,
								output->base.current_mode->width,
								output->base.current_mode->height,
								output->format->format,
								flags);

	if (!output->surface) {
		weston_log("failed to create gbm surface\n");
		return -1;
	}

	output->framebuffer_ubwc = false;
	//Query whether allocated BOs are UBWC or not
	gbm_perform(GBM_PERFORM_GET_SURFACE_UBWC_STATUS, output->surface, &output->framebuffer_ubwc);
	//Check the output secure status
	int secure_status = 0;
	gbm_perform(GBM_PERFORM_GET_SURFACE_SECURE_STATUS, output->surface, &secure_status);
	output->is_secure = (bool)secure_status;


	struct gl_renderer_output_options options = {
		.window_for_legacy = (EGLNativeWindowType) output->surface,
		.window_for_platform = output->surface,
		.formats = format,
		.formats_count = 1,
		.area.x = 0,
		.area.y = 0,
		.area.width = output->base.current_mode->width,
		.area.height = output->base.current_mode->height,
		.fb_size.width = output->base.current_mode->width,
		.fb_size.height = output->base.current_mode->height,
	};

	if (format[1])
		options.formats_count = 2;
	if (gl_renderer->output_window_create(&output->base, &options) < 0) {
		weston_log("failed to create gl renderer output state\n");
		gbm_surface_destroy(output->surface);
		return -1;
	}

	return 0;
}

static int
drm_output_init_pixman(struct drm_output *output, struct drm_backend *b)
{
	struct weston_renderer *renderer = output->base.compositor->renderer;
	const struct pixman_renderer_interface *pixman = renderer->pixman;
	int w = output->base.current_mode->width;
	int h = output->base.current_mode->height;
	unsigned int i;
	const struct pixman_renderer_output_options options = {
		.use_shadow = b->use_pixman_shadow,
		.fb_size = { .width = w, .height = h },
		.format = output->format
	};

	assert(options.format);

	if (!options.format->pixman_format) {
		weston_log("Unsupported pixel format %s\n",
			   options.format->drm_format_name);
		return -1;
	}

	if (pixman->output_create(&output->base, &options))
		goto err;

	/* FIXME error checking */
	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		output->dumb[i] = drm_fb_create_dumb(b, w, h);
		if (!output->dumb[i])
			goto err;

		output->renderbuffer[i] =
			pixman->create_image_from_ptr(&output->base,
						      options.format, w, h,
						      output->dumb[i]->map,
						      output->dumb[i]->stride);
		if (!output->renderbuffer[i])
			goto err;

		pixman_region32_init_rect(&output->renderbuffer[i]->damage,
					output->base.pos.c.x,
					output->base.pos.c.y,
					output->base.width,
					output->base.height);
	}

	weston_log("DRM: output %s %s shadow framebuffer.\n", output->base.name,
		   b->use_pixman_shadow ? "uses" : "does not use");

	return 0;

err:
	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		if (output->dumb[i])
			drm_fb_unref(output->dumb[i]);
		if (output->renderbuffer[i])
			weston_renderbuffer_unref(output->renderbuffer[i]);

		output->dumb[i] = NULL;
		output->renderbuffer[i] = NULL;
	}
	pixman->output_destroy(&output->base);

	return -1;
}

static void
drm_output_fini_pixman(struct drm_output *output)
{
	struct weston_renderer *renderer = output->base.compositor->renderer;
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(output->dumb); i++) {
		weston_renderbuffer_unref(output->renderbuffer[i]);
		drm_fb_unref(output->dumb[i]);
		output->dumb[i] = NULL;
		output->renderbuffer[i] = NULL;
	}

	renderer->pixman->output_destroy(&output->base);
}

static void
edid_parse_string(const uint8_t *data, char text[])
{
	int i;
	int replaced = 0;

	/* this is always 12 bytes, but we can't guarantee it's null
	 * terminated or not junk. */
	strlcpy(text, (const char *) data, 12);

	/* remove insane chars */
	for (i = 0; text[i] != '\0'; i++) {
		if (text[i] == '\n' ||
			text[i] == '\r') {
			text[i] = '\0';
			break;
		}
	}

	/* ensure string is printable */
	for (i = 0; text[i] != '\0'; i++) {
		if (!isprint(text[i])) {
			text[i] = '-';
			replaced++;
		}
	}

	/* if the string is random junk, ignore the string */
	if (replaced > 4)
		text[0] = '\0';
}

#define EDID_DESCRIPTOR_ALPHANUMERIC_DATA_STRING	0xfe
#define EDID_DESCRIPTOR_DISPLAY_PRODUCT_NAME		0xfc
#define EDID_DESCRIPTOR_DISPLAY_PRODUCT_SERIAL_NUMBER	 0xff
#define EDID_OFFSET_DATA_BLOCKS				   0x36
#define EDID_OFFSET_LAST_BLOCK				  0x6c
#define EDID_OFFSET_PNPID				 0x08
#define EDID_OFFSET_SERIAL				  0x0c

static int
edid_parse(struct drm_edid *edid, const uint8_t *data, size_t length)
{
	int i;
	uint32_t serial_number;

	/* check header */
	if (length < 128)
		return -1;
	if (data[0] != 0x00 || data[1] != 0xff)
		return -1;

	/* decode the PNP ID from three 5 bit words packed into 2 bytes
	 * /--08--\/--09--\
	 * 7654321076543210
	 * |\---/\---/\---/
	 * R  C1   C2	C3 */
	edid->pnp_id[0] = 'A' + ((data[EDID_OFFSET_PNPID + 0] & 0x7c) / 4) - 1;
	edid->pnp_id[1] = 'A' + ((data[EDID_OFFSET_PNPID + 0] & 0x3) * 8) + ((data[EDID_OFFSET_PNPID + 1] & 0xe0) / 32) - 1;
	edid->pnp_id[2] = 'A' + (data[EDID_OFFSET_PNPID + 1] & 0x1f) - 1;
	edid->pnp_id[3] = '\0';

	/* maybe there isn't a ASCII serial number descriptor, so use this instead */
	serial_number = (uint32_t) data[EDID_OFFSET_SERIAL + 0];
	serial_number += (uint32_t) data[EDID_OFFSET_SERIAL + 1] * 0x100;
	serial_number += (uint32_t) data[EDID_OFFSET_SERIAL + 2] * 0x10000;
	serial_number += (uint32_t) data[EDID_OFFSET_SERIAL + 3] * 0x1000000;
	if (serial_number > 0)
		snprintf(edid->serial_number, sizeof(edid->serial_number), "%lu", (unsigned long) serial_number);

	/* parse EDID data */
	for (i = EDID_OFFSET_DATA_BLOCKS;
		 i <= EDID_OFFSET_LAST_BLOCK;
		 i += 18) {
		/* ignore pixel clock data */
		if (data[i] != 0)
			continue;
		if (data[i+2] != 0)
			continue;

		/* any useful blocks? */
		if (data[i+3] == EDID_DESCRIPTOR_DISPLAY_PRODUCT_NAME) {
			edid_parse_string(&data[i+5],
					  edid->monitor_name);
		} else if (data[i+3] == EDID_DESCRIPTOR_DISPLAY_PRODUCT_SERIAL_NUMBER) {
			edid_parse_string(&data[i+5],
					  edid->serial_number);
		} else if (data[i+3] == EDID_DESCRIPTOR_ALPHANUMERIC_DATA_STRING) {
			edid_parse_string(&data[i+5],
					  edid->eisa_id);
		}
	}
	return 0;
}

static void
setup_output_seat_constraint(struct drm_backend *b,
		struct weston_output *output,
		const char *s)
{
	if (strcmp(s, "") != 0) {
		struct weston_pointer *pointer;
		struct udev_seat *seat;

		seat = udev_seat_get_named(&b->input, s);
		if (!seat)
			return;

		seat->base.output = output;

		pointer = weston_seat_get_pointer(&seat->base);
		if (pointer)
			pointer->pos = weston_pointer_clamp(pointer,
					pointer->pos);
	}
}

static int
get_gbm_format_from_section(struct weston_config_section *section,
		uint32_t default_value,
		uint32_t *format)
{
	char *s;
	int ret = 0;

	weston_config_section_get_string(section,
			"gbm-format", &s, NULL);

	if (s == NULL)
		*format = default_value;
	else if (strcmp(s, "xrgb8888") == 0)
		*format = GBM_FORMAT_XRGB8888;
	else if (strcmp(s, "rgb565") == 0)
		*format = GBM_FORMAT_RGB565;
	else if (strcmp(s, "xrgb2101010") == 0)
		*format = GBM_FORMAT_XRGB2101010;
	else {
		weston_log("fatal: unrecognized pixel format: %s\n", s);
		ret = -1;
	}

	free(s);

	return ret;
}

static int
headless_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, WP_PRESENTATION_FEEDBACK_INVALID);

	return 0;
}

static int
finish_frame_handler(void *data)
{
	struct drm_output *output = data;

	weston_output_repaint_failed(&output->base);

	return 1;
}

static int
headless_output_repaint(struct weston_output *output_base,
			   pixman_region32_t *damage)
{
	struct drm_output *output = (struct drm_output *) output_base;
	struct weston_compositor *ec = output->base.compositor;

	wl_event_source_timer_update(output->finish_frame_timer, 16);

	return 0;
}

static void get_display_config(int idx, struct DisplayConfigInfo *display_config)
{
	/* default 1080p, 60 fps */
	display_config->x_pixels = 1920;
	display_config->y_pixels = 1080;
	display_config->x_dpi = 300;
	display_config->y_dpi = 300;
	display_config->fps = 60;
	display_config->vsync_period_ns = 16600000;
	display_config->is_yuv = false;
	bool rc = sdm_service->GetDisplayConfiguration(idx, display_config);
	if (!rc) {
		weston_log("Fail to get preferred mode, use default mode instead!");
	}
}

static void update_head(struct drm_head *head, struct DisplayConfigInfo *display_config)
{
	uint32_t mmWidth  = (display_config->x_pixels / display_config->x_dpi) * 25.4;
	uint32_t mmHeight = (display_config->y_pixels / display_config->y_dpi) * 25.4;
	weston_head_set_subpixel(&head->base, WL_OUTPUT_SUBPIXEL_UNKNOWN);
	weston_head_set_physical_size(&head->base, mmWidth, mmHeight);
	head->inherited_mode.hdisplay = display_config->x_pixels;
	head->inherited_mode.vdisplay = display_config->y_pixels;
	head->inherited_mode.vrefresh = display_config->fps * 1000;
	head->inherited_mode.flags |= WL_OUTPUT_MODE_CURRENT;
}

static void
hotplug_handler(int disp, bool connected, struct drm_head *head)
{
	struct DisplayConfigInfo display_config = { 0 };

	if (head == NULL) {
		weston_log("head is null in hotplug handler\n");
		return;
	}

	drm_debug(head->backend, "[hotplug] connect %d\n", connected);
	if (connected) {
		get_display_config(head->display_id, &display_config);

		update_head(head, &display_config);
	}

	weston_head_set_connection_status(&head->base, connected);
}

static int
drm_output_attach_head(struct weston_output *output_base,
		struct weston_head *head_base)
{
	struct drm_backend *b = to_drm_backend(output_base->compositor);

	if (wl_list_length(&output_base->head_list) >= MAX_CLONED_CONNECTORS)
		return -1;

	if (!output_base->enabled)
		return 0;

	/* XXX: ensure the configuration will work.
	 * This is actually impossible without major infrastructure
	 * work. */

	/* Need to go through modeset to add connectors. */
	/* XXX: Ideally we'd do this per-output, not globally. */
	/* XXX: Doing it globally, what guarantees another output's update
	 * will not clear the flag before this output is updated?
	 */
	//b->state_invalid = true;

	weston_output_schedule_repaint(output_base);

	return 0;
}

static void
drm_output_detach_head(struct weston_output *output_base,
		struct weston_head *head_base)
{
	struct drm_backend *b = to_drm_backend(output_base->compositor);

	if (!output_base->enabled)
		return;

	/* Need to go through modeset to drop connectors that should no longer
	 * be driven.
	 */
	weston_output_schedule_repaint(output_base);
}

static int
parse_gbm_format(const char *s, const struct pixel_format_info *default_format,
		 const struct pixel_format_info **format)
{
	if (s == NULL) {
		*format = default_format;

		return 0;
	}

	/* GBM formats and DRM formats are identical. */
	*format = pixel_format_get_info_by_drm_name(s);
	if (!*format) {
		weston_log("fatal: unrecognized pixel format: %s\n", s);

		return -1;
	}

	return 0;
}

static int
drm_output_set_mode(struct weston_output *base,
		enum weston_drm_backend_output_mode mode,
		const char *modeline)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_backend *b = to_drm_backend(base->compositor);
	struct drm_head *head = to_drm_head(weston_output_get_first_head(base));
	struct drm_mode *current;

	output->display_id = head->display_id;
	output->early_display_enable = head->early_display_enable;
	output->early_display_intf =  head->early_display_intf;
	/*don't consider clone mode first*/
	current = zalloc(sizeof *current);
	if (!current)
		return -1;
	/* only one static mode in list */
	current->base.width   = head->inherited_mode.hdisplay;
	current->base.height  = head->inherited_mode.vdisplay;
	current->base.refresh = head->inherited_mode.vrefresh;
	output->base.current_mode = &current->base;
	output->base.current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &current->base.link);
	output->base.current_mode = &current->base;
	output->base.current_mode->flags |= WL_OUTPUT_MODE_CURRENT;

	/* Set native_ fields, so weston_output_mode_switch_to_native() works */
	output->base.native_mode = output->base.current_mode;
	output->base.native_scale = output->base.current_scale;

	return 0;
}

static void
drm_output_set_gbm_format(struct weston_output *base,
		const char *gbm_format)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_backend *b = to_drm_backend(base->compositor);

	if (parse_gbm_format(gbm_format, b->format, &output->format) == -1)
		output->format = b->format;

	/* Without universal planes, we can't discover which formats are
	 * supported by the primary plane; we just hope that the GBM format
	 * works. */
	//if (!b->universal_planes)
	//	output->scanout_plane->formats[0].format = output->gbm_format;
}

static void
drm_output_set_seat(struct weston_output *base,
		const char *seat)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_backend *b = to_drm_backend(base->compositor);

	setup_output_seat_constraint(b, &output->base,
			seat ? seat : "");
}

static void
drm_output_set_max_bpc(struct weston_output *base, unsigned max_bpc)
{
}

static int
drm_output_set_content_type(struct weston_output *base,
			    const char *content_type)
{
	return 0;
}

static void
drm_head_log_info(struct drm_head *head, const char *msg)
{
	if (head->base.connected) {
		weston_log("DRM: head '%s' %s, connector %d is connected, "
				"EDID make '%s', model '%s', serial '%s'\n",
				head->base.name, msg, head->connector_id,
				head->base.make, head->base.model,
				head->base.serial_number ?: "");
	} else {
		weston_log("DRM: head '%s' %s, connector %d is disconnected.\n",
				head->base.name, msg, head->connector_id);
	}
}

/**
 * Create a Weston head for a connector
 *
 * Given a DRM connector, create a matching drm_head structure and add it
 * to Weston's head list.
 *
 * @param b Weston backend structure
 * @param dispplay_id display ID for the head
 * @param display_config display info
 * @param connector_id connector id
 * @returns The new head, or NULL on failure.
 */
static struct drm_head *
drm_head_create(struct drm_backend *backend, uint32_t display_id,
		struct DisplayConfigInfo *display_config, int connector_id)
{
	struct drm_head *head;
	char *name;

	head = zalloc(sizeof *head);
	if (!head)
		return NULL;

	name = sdm_service->GetConnectorName(display_id);
	if (!name)
		goto err_alloc;

	weston_head_init(&head->base, name);

	head->display_id = display_id;
	head->connector_id = connector_id;
	head->backend = backend;
	head->backlight = BACKLIGHT_RAW;
	weston_head_set_internal(&head->base);
	weston_head_set_connection_status(&head->base, true);
	weston_head_set_monitor_strings(&head->base, "unknown",
			name, NULL);
	free(name);

	head->base.backend = &backend->base;

	uint32_t mmWidth  = (display_config->x_pixels/display_config->x_dpi)*25.4;
	uint32_t mmHeight = (display_config->y_pixels/display_config->y_dpi)*25.4;
	weston_head_set_subpixel(&head->base, WL_OUTPUT_SUBPIXEL_UNKNOWN);
	weston_head_set_physical_size(&head->base, mmWidth,
			mmHeight);

	head->inherited_mode.hdisplay	= display_config->x_pixels;
	head->inherited_mode.vdisplay	= display_config->y_pixels;
	head->inherited_mode.vrefresh = display_config->fps * 1000;
	head->inherited_mode.flags |= WL_OUTPUT_MODE_CURRENT;

	weston_compositor_add_head(backend->compositor, &head->base);
	drm_head_log_info(head, "found");

	return head;

err_alloc:
	free(head);

	return NULL;
}

/**
 * Create a Weston head for a connector
 *
 * Given a DRM connector, create a matching drm_head structure and add it
 * to Weston's head list.
 *
 * @param b Weston backend structure
 * @param dispplay_id display ID for the head
 * @param display_config display info
 * @returns The new head, or NULL on failure.
 */
static struct drm_head *
drm_head_create_early(struct drm_backend *backend, uint32_t display_id,struct EarlyDisplayInfo *display_config)
{
	struct drm_head *head;
	char *name;

	head = zalloc(sizeof *head);
	if (!head)
		return NULL;

	name = display_config->name;
	if (!name)
		goto err_alloc;

	weston_head_init(&head->base, name);

	head->display_id = display_id;
	head->connector_id = early_get_connector_id(display_id);
	head->backend = backend;
	head->early_display_enable = display_config->early_enable;
	head->early_display_intf = display_config->early_diplay_intf;
	head->backlight = BACKLIGHT_RAW;
	weston_head_set_internal(&head->base);
	weston_head_set_connection_status(&head->base, true);
	weston_head_set_monitor_strings(&head->base, "unknown",
			name, NULL);
	free(name);

	head->base.backend = &backend->base;

	uint32_t mmWidth  = display_config->x_pixels;
	uint32_t mmHeight = display_config->y_pixels;
	weston_head_set_subpixel(&head->base, WL_OUTPUT_SUBPIXEL_UNKNOWN);
	weston_head_set_physical_size(&head->base, mmWidth,
			mmHeight);

	head->inherited_mode.hdisplay	= display_config->x_pixels;
	head->inherited_mode.vdisplay	= display_config->y_pixels;
	head->inherited_mode.vrefresh = display_config->fps * 1000;
	head->inherited_mode.flags |= WL_OUTPUT_MODE_CURRENT;

	weston_compositor_add_head(backend->compositor, &head->base);
	drm_head_log_info(head, "found");

	return head;

err_alloc:
	free(head);

	return NULL;
}

static void
drm_head_destroy(struct drm_head *head)
{
	weston_head_release(&head->base);

	if (head->early_display_intf)
		early_destroy_display(head->early_display_intf);
	if (head->backlight)
		backlight_destroy(head->backlight);

	free(head);
}

static void
drm_display_destroy(struct drm_head *head)
{
	if (head->early_display_intf)
		early_destroy_display(head->early_display_intf);
	if (sdm_service)
		sdm_service->DestroyDisplay(head->display_id);
}

static void
drm_output_print_modes(struct drm_output *output)
{
	struct weston_mode *m;
	int count_modes = 1;

	weston_log("Output name %s\n", output->base.name);
	   wl_list_for_each(m, &output->base.mode_list, link)
		   weston_log_continue(STAMP_SPACE "mode %dx%d@%.1f%s%s%s\n",
					m->width, m->height, m->refresh / 1000.0,
					m->flags & WL_OUTPUT_MODE_PREFERRED ?
					", preferred" : "",
					m->flags & WL_OUTPUT_MODE_CURRENT ?
					", current" : "",
					count_modes == 0 ?
					", built-in" : "");

}

static int
drm_output_enable(struct weston_output *base)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_backend *b = to_drm_backend(base->compositor);

	int ret;

	wl_list_init(&output->plane_flip_list);
	wl_list_init(&output->sdm_layer_list);
	wl_list_init(&output->early_layer_list);
	wl_list_init(&output->commited_layer_list);
	wl_list_init(&output->commited_early_list);

	if (b->sdm_repaint) {
		if (b->use_pixman) {
			if (drm_output_init_pixman(output, b) < 0) {
				weston_log("Failed to init output pixman state\n");
				goto err;
			}
		} else if (drm_output_init_egl(output, b) < 0) {
			weston_log("Failed to init output gl state\n");
			goto err;
		}
		drm_set_dpms(&output->base, WESTON_DPMS_ON);
	}

	output->base.start_repaint_loop = drm_output_start_repaint_loop;
	output->base.repaint = drm_output_repaint;
	output->base.assign_planes = drm_assign_planes;
	output->base.set_dpms = drm_set_dpms;
	output->base.switch_mode = drm_output_switch_mode;
	output->base.gamma_size = 0;
	output->base.set_gamma = NULL;
	output->base.enable_ppm = drm_enable_ppm;
	output->base.set_ppm = drm_set_ppm;

	drm_output_print_modes(output);
	output->prev_layer_none_commit = true;
	output->layer_none_commit = true;
	output->retire_fence_fd = -1;

	return 0;

err:

	return -1;
}

static void
drm_output_fini_egl(struct drm_output *output)
{
	struct drm_backend *b = to_drm_backend(output->base.compositor);

	/* Destroying the GBM surface will destroy all our GBM buffers,
	 * regardless of refcount. Ensure we destroy them here.
	 */
	gl_renderer->output_destroy(&output->base);
	gbm_surface_destroy(output->surface);
}

static void
drm_output_deinit(struct weston_output *base)
{
	struct drm_output *output = to_drm_output(base);
	struct drm_backend *b = to_drm_backend(base->compositor);

	if (b->use_pixman)
		drm_output_fini_pixman(output);
	else
		drm_output_fini_egl(output);
}

static int
drm_output_disable(struct weston_output *base)
{
	struct drm_output *output = to_drm_output(base);

	if (output->frame_pending) {
		output->disable_pending = 1;
		return -1;
	}

	weston_log("Disabling output %s\n", output->base.name);

	if (output->base.enabled)
		drm_output_deinit(&output->base);

	output->disable_pending = 0;

	return 0;
}


/**
 * Create a Weston output structure
 *
 * Create an "empty" drm_output. This is the implementation of
 * weston_backend::create_output.
 *
 * Creating an output is usually followed by drm_output_attach_head()
 * and drm_output_enable() to make use of it.
 *
 * @param compositor The compositor instance.
 * @param name Name for the new output.
 * @returns The output, or NULL on failure.
 */
static struct weston_output *
drm_output_create(struct weston_backend *backend, const char *name)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct drm_output *output;

	output = zalloc(sizeof *output);
	if (output == NULL)
		return NULL;

	weston_output_init(&output->base, b->compositor, name);

	output->base.enable = drm_output_enable;
	output->base.destroy = drm_output_destroy;
	output->base.disable = drm_output_disable;
	output->base.attach_head = drm_output_attach_head;
	output->base.detach_head = drm_output_detach_head;

	output->destroy_pending = 0;
	output->disable_pending = 0;
	output->frame_pending = 0;

	struct wl_event_loop *loop = wl_display_get_event_loop(b->compositor->wl_display);
	output->finish_frame_timer =
			wl_event_loop_add_timer(loop, finish_frame_handler, output);

	weston_compositor_add_pending_output(&output->base, b->compositor);

#if ENABLE_PAGEFLIP
	if (0 > drm_output_enable_pageflip(output)) {
		weston_log("Failed to create pageflip event\n");
		return NULL;
	}
#endif

	return &output->base;
}

static struct drm_head *
drm_head_find_by_connector(struct drm_backend *backend, uint32_t connector_id)
{
		struct weston_head *base;
		struct drm_head *head;

		wl_list_for_each(base,
				&backend->compositor->head_list, compositor_link) {
			head = to_drm_head(base);
			if (head->connector_id == connector_id)
				return head;
		}

		return NULL;
}

static int
drm_backend_create_sdm_heads(struct drm_backend *b)
{
	uint32_t display_count = 0;
	int idx, rc;
	struct drm_head *head = NULL;

	if (sdm_service->GetDisplayInfos()) {
		weston_log("fail to get display info from SDM!\n");
		return -1;
	}

	display_count = sdm_service->GetDisplayCount();
	weston_log("%d displays are connected\n", display_count);

	for (idx = 0; idx < display_count; idx++) {

		/* and create default display */
		create_sdm_display(b, idx);

		/* Now register callbacks with SDM services */
	}

	return 0;
}

static int
drm_backend_create_heads(struct drm_backend *b)
{
	uint32_t conn_count = 0, idx;
	int x=0, y=0;
	int rc;
	struct drm_output *output;
	struct drm_head *head;

	early_get_connector_count(&conn_count);
	if (!conn_count) {
		weston_log("fail to get connector num \n");
	}

	weston_log("create_outputs_early: conn_count=%d\n", conn_count);

	for (idx = 0; idx < conn_count; idx++) {
		 struct EarlyDisplayInfo display_config = {};
		/*
		 * Only enable early display for primary output to
		 * meet boot KPI
		 */
		if (idx == 0)
			display_config.early_enable = true;
		else
			display_config.early_enable = false;
		rc = early_create_display(idx, &display_config);
		if (rc) {
			weston_log("CreateDisplay: %d return %d\n", idx, rc);
			goto err;
		}

		if(!drm_head_create_early(b, idx, &display_config)) {
			weston_log("Create weston head for display %d failed\n", idx);
			early_destroy_display(display_config.early_diplay_intf);
			goto err;
		}
		x += display_config.x_pixels;
	}


	/* Unregister early displays in order not to block SDM register
	 * displays
	 */
	wl_list_for_each(head, &b->compositor->head_list, base.compositor_link)
		early_unregister_display(head->early_display_intf);

	return 0;

err:
	wl_list_for_each(head, &b->compositor->head_list, base.compositor_link)
		early_destroy_display(head->early_display_intf);

	return -1;
}

static void
update_outputs(struct drm_backend *b, struct udev_device *drm_device)
{
	int x = 0, y = 0;

	struct weston_output *last = container_of(b->compositor->output_list.prev,
											struct weston_output, link);

	/* XXX: not yet needed, we die with 0 outputs */
	if (!wl_list_empty(&b->compositor->output_list))
		x = last->pos.c.x + last->width;
	else
		x = 0;
	y = 0;

	/* TODO: How to handle it even though hotplug is not supported?
	 * Fake a config first
	 */
	struct DisplayConfigInfo display_config;
	display_config.x_pixels		   = 1920;
	display_config.y_pixels		   = 1080;
	display_config.x_dpi		   = 96.0f;
	display_config.y_dpi		   = 96.0f;
	display_config.fps			   = 60;
	display_config.vsync_period_ns = 0;
	display_config.is_yuv		   = false;
}

static int
udev_event_is_hotplug(struct drm_backend *b, struct udev_device *device)
{
	const char *sysnum;
	const char *val;
	const char *stat;

	sysnum = udev_device_get_sysnum(device);
	if (!sysnum || atoi(sysnum) != b->drm.id)
		return 0;

	val = udev_device_get_property_value(device, "HOTPLUG");
	if (!val) {
		val = udev_device_get_property_value(device, "MST_HOTPLUG");
		return 0;
	}

	return strcmp(val, "1") == 0;
}

static int
udev_event_is_conn_prop_change(struct drm_backend *b,
			       struct udev_device *device,
			       uint32_t *connector_id,
			       uint32_t *property_id)
{
	const char *val;
	int id;

	val = udev_device_get_property_value(device, "CONNECTOR");
	if (!val || !safe_strtoint(val, &id))
		return 0;
	else
		*connector_id = id;

	val = udev_device_get_property_value(device, "PROPERTY");
	if (!val || !safe_strtoint(val, &id))
		return 0;
	else
		*property_id = id;

	return 1;
}
static int create_sdm_display(struct drm_backend *backend, int idx)
{
	sdm_cbs_t sdm_cbs;
	struct DisplayConfigInfo display_config = {0};
	struct drm_head* head = NULL;
	uint32_t connector_id;
	int rc;

	rc = sdm_service->CreateDisplay(idx);
	if (!rc)
		weston_log("CreateDisplay: %d successful\n", idx);
	else {
		weston_log("CreateDisplay: %d fail\n", idx);
		goto err_stat;
	}

	sdm_cbs.hotplug_cb = hotplug_handler,
	sdm_cbs.pageflip_cb = (pageflip_cb_t)pageflip_handler;
	sdm_service->RegisterCbs(idx, &sdm_cbs);
	connector_id = sdm_service->GetConnectorId(idx);
	head = drm_head_find_by_connector(backend, connector_id);
	if (head && head->display_id == idx)
		return 0;
	else if (head) {
		weston_log(" display_id %d is different from early display_id %d\n", idx, head->display_id);
		drm_head_destroy(head);
	}

	get_display_config(idx, &display_config);
	head = drm_head_create(backend, idx, &display_config, connector_id);
	if(!head) {
		sdm_service->DestroyDisplay(idx);
		goto err_stat;
	}

	sdm_service->SetHead(idx, head);
	return 0;

err_stat:
	sdm_service->UpdateDisplayStatus(idx, false);
	return -1;
}
udev_drm_event(int fd, uint32_t mask, void *data)
{
	struct drm_backend *b = data;
	struct udev_device *event;
	uint32_t conn_id, prop_id;
	uint32_t new_plugs;
	int display_id = 0;
	int ret = -1;

	event = udev_monitor_receive_device(b->udev_monitor);
	/* TODO (user): Need to hook this with SDM for hotplug support. */
	// TODO (user): if (udev_event_is_hotplug(b, event))
	// TODO (user):		update_outputs(b, event);
	if (udev_event_is_hotplug(b, event)) {
		if (udev_event_is_conn_prop_change(b, event, &conn_id, &prop_id)) {
			weston_log("property change is not supported\n");
		} else {
			weston_log("hotplug happen\n");
			new_plugs = sdm_service->UpdateDisplayInfos();
			if (new_plugs > 0) {
				weston_log("have new plug in %d displays\n", new_plugs);
				for (int i = 0; i < new_plugs; i++) {
					display_id = sdm_service->GetNewPlugDisplayID(i);
					ret = create_sdm_display(b, display_id);
				}
			}
		}
	}
	udev_device_unref(event);

	return 1;
}

static void
drm_destroy(struct weston_backend *backend)
{
	struct drm_backend *b = container_of(backend, struct drm_backend, base);
	struct weston_compositor *ec = b->compositor;
	struct weston_head *base, *next;

	udev_input_destroy(&b->input);

	wl_event_source_remove(b->udev_drm_source);
	weston_log_scope_destroy(b->debug);
	b->debug = NULL;
	//TODO(user): Need to destroy the display device here
	wl_list_for_each_safe(base, next, &ec->head_list, compositor_link) {
		drm_display_destroy(to_drm_head(base));
		drm_head_destroy(to_drm_head(base));
	}
	/* This will destroy all displays also */
	if (sdm_service)
		sdm_service->DestroyCore();
	unload_touch_only_surface_module();
	if(b->early_boot)
		early_drm_display_deinit(true, b->drm.fd);

	if (b->gbm)
		gbm_device_destroy(b->gbm);

	weston_launcher_destroy(ec->launcher);

	close(b->drm.fd);
	//close(b->render_fd);
	drmClose(b->render_fd);
	free(b->drm.filename);
	free(b);
}

static void
drm_backend_set_modes(struct drm_backend *backend)
{
	struct drm_output *output;
	struct drm_mode *drm_mode;
	int ret;

	wl_list_for_each(output, &backend->compositor->output_list, base.link) {
		if (!output->current) {
			/* If something that would cause the output to
			 * switch mode happened while in another vt, we
			 * might not have a current drm_fb. In that case,
			 * schedule a repaint and let drm_output_repaint
			 * handle setting the mode. */
			weston_output_schedule_repaint(&output->base);
			continue;
		}

		drm_mode = (struct drm_mode *) output->base.current_mode;
		ret = drmModeSetCrtc(backend->drm.fd, output->crtc_id,
						output->current->fb_id, 0, 0,
						&output->connector_id, 1,
						&drm_mode->mode_info);
		if (ret < 0) {
			weston_log(
				"failed to set mode %dx%d for output at %d,%d: %m\n",
				drm_mode->base.width, drm_mode->base.height,
				output->base.pos.c.x, output->base.pos.c.y);
		}
	}
}

static void
session_notify(struct wl_listener *listener, void *data)
{
	struct weston_compositor *compositor = data;
	struct drm_backend *b = to_drm_backend(compositor);
	struct drm_sprite *sprite;
	struct drm_output *output;

	if (compositor->session_active) {
		weston_log("activating session\n");
		compositor->state = b->prev_state;
		drm_backend_set_modes(b);
		weston_compositor_damage_all(compositor);
		udev_input_enable(&b->input);
	} else {
		weston_log("deactivating session\n");
		udev_input_disable(&b->input);

		b->prev_state = compositor->state;
		weston_compositor_offscreen(compositor);

		/* If we have a repaint scheduled (either from a
		 * pending pageflip or the idle handler), make sure we
		 * cancel that so we don't try to pageflip when we're
		 * vt switched away.  The OFFSCREEN state will prevent
		 * further attemps at repainting.  When we switch
		 * back, we schedule a repaint, which will process
		 * pending frame callbacks. */

		wl_list_for_each(output, &compositor->output_list, base.link) {
			output->base.repaint_needed = 0;
			drmModeSetCursor(b->drm.fd, output->crtc_id, 0, 0, 0);
		}

		output = container_of(compositor->output_list.next,
							struct drm_output, base.link);

	};
}

static void
switch_vt_binding(struct weston_keyboard *keyboard, const struct timespec *time,
		uint32_t key, void *data)
{
	struct weston_compositor *compositor = data;

	weston_launcher_activate_vt(compositor->launcher, key - KEY_F1 + 1);
}

#ifdef BUILD_VAAPI_RECORDER
static void
recorder_destroy(struct drm_output *output)
{
	vaapi_recorder_destroy(output->recorder);
	output->recorder = NULL;

	output->base.disable_planes--;

	wl_list_remove(&output->recorder_frame_listener.link);
	weston_log("[libva recorder] done\n");
}

static void
recorder_frame_notify(struct wl_listener *listener, void *data)
{
	struct drm_output *output;
	struct drm_backend *b;
	int fd, ret;

	output = container_of(listener, struct drm_output,
						recorder_frame_listener);
	b = to_drm_backend(output->base.compositor);

	if (!output->recorder)
		return;

	ret = drmPrimeHandleToFD(b->drm.fd, output->current->handle,
					DRM_CLOEXEC, &fd);
	if (ret) {
		weston_log("[libva recorder] "
			   "failed to create prime fd for front buffer\n");
		return;
	}

	ret = vaapi_recorder_frame(output->recorder, fd,
					output->current->stride);
	if (ret < 0) {
		weston_log("[libva recorder] aborted: %m\n");
		recorder_destroy(output);
	}
}

static void *
create_recorder(struct drm_backend *b, int width, int height,
		const char *filename)
{
	int fd;
	drm_magic_t magic;

	fd = drmOpen(b->drm.filename, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	drmGetMagic(fd, &magic);
	drmAuthMagic(b->drm.fd, magic);

	return vaapi_recorder_create(fd, width, height, filename);
}

static void
recorder_binding(struct weston_keyboard *keyboard, const struct timespec *time, uint32_t key,
		void *data)
{
	struct drm_backend *b = data;
	struct drm_output *output;
	int width, height;

	output = container_of(b->compositor->output_list.next,
						struct drm_output, base.link);

	if (!output->recorder) {
		if (output->format != GBM_FORMAT_XRGB8888) {
			weston_log("failed to start vaapi recorder: "
					"output format not supported\n");
			return;
		}

		width = output->base.current_mode->width;
		height = output->base.current_mode->height;

		output->recorder =
			create_recorder(b, width, height, "capture.h264");
		if (!output->recorder) {
			weston_log("failed to create vaapi recorder\n");
			return;
		}

		output->base.disable_planes++;

		output->recorder_frame_listener.notify = recorder_frame_notify;
		wl_signal_add(&output->base.frame_signal,
				&output->recorder_frame_listener);

		weston_output_schedule_repaint(&output->base);

		weston_log("[libva recorder] initialized\n");
	} else {
		recorder_destroy(output);
	}
}
#else
static void
recorder_binding(struct weston_keyboard *keyboard, const struct timespec *time, uint32_t key,
		void *data)
{
	weston_log("Compiled without libva support\n");
}
#endif

static void
switch_to_gl_renderer(struct drm_backend *b)
{
	struct drm_output *output;
	bool dmabuf_support_inited;

	if (!b->use_pixman)
		return;

	dmabuf_support_inited = !!b->compositor->renderer->import_dmabuf;

	weston_log("Switching to GL renderer\n");

	b->gbm = create_gbm_device(b->render_fd);
	if (!b->gbm) {
		weston_log("Failed to create gbm device. "
				"Aborting renderer switch\n");
		return;
	}

	wl_list_for_each(output, &b->compositor->output_list, base.link)
		b->compositor->renderer->pixman->output_destroy(&output->base);

	b->compositor->renderer->destroy(b->compositor);

	if (drm_backend_create_gl_renderer(b) < 0) {
		gbm_device_destroy(b->gbm);
		weston_log("Failed to create GL renderer. Quitting.\n");
		/* FIXME: we need a function to shutdown cleanly */
		assert(0);
	}

	wl_list_for_each(output, &b->compositor->output_list, base.link)
		drm_output_init_egl(output, b);

	b->use_pixman = 0;

	if (!dmabuf_support_inited && b->compositor->renderer->import_dmabuf) {
		if (linux_dmabuf_setup(b->compositor) < 0)
			weston_log("Error: initializing dmabuf "
					"support failed.\n");
	}
}

static void
renderer_switch_binding(struct weston_keyboard *keyboard, const struct timespec *time,
		uint32_t key, void *data)
{
	struct drm_backend *b =
		(struct drm_backend *) keyboard->seat->compositor;

	switch_to_gl_renderer(b);
}

static const struct weston_drm_output_api api = {
	drm_output_set_mode,
	drm_output_set_gbm_format,
	drm_output_set_seat,
	drm_output_set_max_bpc,
	drm_output_set_content_type,
};

static int init_sdm(void) {
	/*
	* Sdm service is built seperately and loaded dynamically
	* to reduce loading time of drm-backend.so
	*/
	sdm_service = weston_load_module("sdm-service.so",
							"sdm_service_interface", LIBWESTON_MODULEDIR);
	if (!sdm_service)
		return -1;

	int rc = sdm_service->CreateCore();
	if (rc) {
		weston_log("failed to create SDM core, error = %d\n", rc);
		return -1;
	}

	weston_log("SDM core created\n");
	return 0;
}

struct udev_para {
	struct udev_input *input;
	struct weston_compositor *compositor;
	struct udev *udev;
	const char *seat_id;
	void (*configure_device)(struct weston_compositor *compositor,
					struct libinput_device *device);
};

static int bg_init_input(void *arg)
{
	struct udev_para *para = (struct udev_para*)arg;
	struct drm_backend *b = to_drm_backend(para->compositor);

	if (udev_input_init(para->input,
			para->compositor, para->udev, para->seat_id, para->configure_device)) {
		weston_log("udev input init failed\n");
	}

	wl_event_source_remove(b->input_init);
	free(para);

	return 0;
}

/*
* Arguments passed to full init main function
*/
struct full_init_param {
	struct drm_backend *b;
	const char *seat_id;
	void (*configure_device)(struct weston_compositor *compositor,
								 struct libinput_device *device);
	/*
	* If early boot is not enabled, need to return
	* whether full init is done successfully
	*/
	bool success;
};

static void *full_init_main(void *arg) {
	struct full_init_param *param =
			(struct full_init_param *)arg;
	struct drm_backend *b = param->b;
	struct wl_event_loop *loop;
	uint32_t key;
	struct udev_para *para = NULL;
	const char* seat_id = param->seat_id;

	if (b->early_boot && b->first_repaint) {
		weston_log("full init thread enter\n");
		pthread_detach(pthread_self());
		/*
		 * In order to display first frame as early as possible,
		 * full init thread does not start to do initialization
		 * until first output repaint is done
		 */
		pthread_mutex_lock(&full_init_mutex);
		if (b->first_repaint)
			pthread_cond_wait(&full_init_cond, &full_init_mutex);
		pthread_mutex_unlock(&full_init_mutex);
	}
	weston_log("begin full init\n");

	struct weston_compositor *compositor
			= b->compositor;

	b->udev = udev_new();
	if (b->udev == NULL) {
		weston_log("failed to initialize udev context\n");
		goto err_base;
	}

	b->session_listener.notify = session_notify;
	wl_signal_add(&compositor->session_signal, &b->session_listener);

	if(init_sdm() < 0)
		goto err_udev;

	if(drm_backend_create_sdm_heads(b))
		goto err_sdm_core;

	gl_renderer = weston_load_module("gl-renderer.so",
							"gl_renderer_interface", LIBWESTON_MODULEDIR);
	if (!gl_renderer)
		goto err_sdm_core;

	/* GBM will load a dri driver, but even though they need symbols from
	 * libglapi, in some version of Mesa they are not linked to it. Since
	 * only the gl-renderer module links to it, the call above won't make
	 * these symbols globally available, and loading the DRI driver fails.
	 * Workaround this by dlopen()'ing libglapi with RTLD_GLOBAL. */
	dlopen("libglapi.so.0", RTLD_LAZY | RTLD_GLOBAL);

	b->prev_state = WESTON_COMPOSITOR_ACTIVE;

	for (key = KEY_F1; key < KEY_F9; key++)
		weston_compositor_add_key_binding(compositor, key, MODIFIER_CTRL | MODIFIER_ALT,
				switch_vt_binding, compositor);

	b->udev_monitor = udev_monitor_new_from_netlink(b->udev, "udev");
	if (b->udev_monitor == NULL) {
		weston_log("failed to intialize udev monitor\n");
		goto err_sdm_core;
	}
	udev_monitor_filter_add_match_subsystem_devtype(b->udev_monitor,
			"drm", NULL);

	loop = wl_display_get_event_loop(compositor->wl_display);
	b->udev_drm_source =
		wl_event_loop_add_fd(loop,
				udev_monitor_get_fd(b->udev_monitor),
				WL_EVENT_READABLE, udev_drm_event, b);
	if (!b->udev_drm_source) {
		weston_log("failed to add wl-event-loop fd\n");
		goto err_udev_monitor;
	}

	if (udev_monitor_enable_receiving(b->udev_monitor) < 0) {
		weston_log("failed to enable udev-monitor receiving\n");
		goto err_udev_drm_source;
	}

	weston_compositor_add_debug_binding(compositor, KEY_Q,
			recorder_binding, b);
	weston_compositor_add_debug_binding(compositor, KEY_W,
			renderer_switch_binding, b);
	if (b->early_boot) {
		/*
		 * In early perf image, we need to postpone udev
		 * input initialization, or it fails because udev are
		 * not full ready
		 */
		para = (struct udev_para *)malloc(sizeof(struct udev_para));
		if (!para) {
			weston_log("out of memory\n");
			goto err_udev_drm_source;
		}
		para->input = &b->input;
		para->compositor = b->compositor;
		para->udev = b->udev;
		para->seat_id = param->seat_id;
		para->configure_device = param->configure_device;
		b->input_init = wl_event_loop_add_timer(loop, bg_init_input, para);
		if (!b->input_init) {
			weston_log("failed to add wl-event-loop input_init timer\n");
			goto err_free_para;
		}

		/*
		 * Set the timer with empirical time cost for systemd
		 * initialization, will refine it with a final solution
		 * in future
		 */
		wl_event_source_timer_update(b->input_init, 2000);

		/*
		* Set up a timer to switch to sdm repaint mode
		*/
		b->finish_full_init = wl_event_loop_add_timer(loop, finish_init, b);
		if (!b->finish_full_init) {
			weston_log("failed to add wl-event-loop finish_init timer\n");
			goto err_wl_event_input_init;
		}
		wl_event_source_timer_update(b->finish_full_init, 1);

		free(param);
		return NULL;
	}

	if (udev_input_init(&b->input, b->compositor, b->udev, param->seat_id, param->configure_device) < 0) {
		weston_log("failed to create input devices\n");
		goto err_free_para;
	}

	if (finish_init(b))
		goto err_udev_input;
	param->success = true;
	return NULL;

err_udev_input:
	udev_input_destroy(&b->input);
err_wl_event_input_init:
	if (b->input_init)
		wl_event_source_remove(b->input_init);
err_free_para:
	if (para)
		free(para);
err_udev_drm_source:
	wl_event_source_remove(b->udev_drm_source);
err_udev_monitor:
	udev_monitor_unref(b->udev_monitor);
err_sdm_core:
	sdm_service->DestroyCore();
err_udev:
	udev_unref(b->udev);
err_base:
	if (b->early_boot)
		free(param);
	else
		param->success = false;
	return NULL;
}

static struct drm_backend *
drm_backend_create(struct weston_compositor *compositor,
		   struct weston_drm_backend_config *config)
{
	struct drm_backend *b;
	struct udev_device *drm_device;
	const char *seat_id = default_seat;
	const char *session_seat;
	int ret;
	struct weston_head *base, *next;

#if defined(ENABLE_RT_SCHEDULE)
	CPUConfigReq_t     cpuConfigReq  = {0};
	CPUConfigResp_t    cpuConfigResp = {0};
	const char        *procName      = "display";
	const char        *thrdGrpName   = "SDM_compositor";
	struct sched_param params;
#endif

	session_seat = getenv("XDG_SEAT");
	if (session_seat)
		seat_id = session_seat;

	if (config->seat_id)
		seat_id = config->seat_id;

	weston_log("initializing drm backend\n");

	b = zalloc(sizeof *b);
	if (b == NULL)
		return NULL;

	b->compositor = compositor;
	b->use_pixman = (config->renderer == WESTON_RENDERER_PIXMAN) ? true : false;
	b->use_pixman_shadow = config->use_pixman_shadow;

	b->debug = weston_compositor_add_log_scope(compositor,
						   "drm-backend",
						   "Debug messages from DRM/KMS backend\n",
						    NULL, NULL, NULL);

	wl_list_insert(&compositor->backend_list, &b->base.link);

	/* Framebuffer should be in ARGB format to support mixed mode composition
	 * e.g., if framebuffer is sandwiched between application views where in
	 * these application views are composed by overlays/SDE.
	 */
	if (parse_gbm_format(config->gbm_format,
				pixel_format_get_info(GBM_FORMAT_ABGR8888),
				&b->format) < 0)
		goto err_base;

	/* Check if we run drm-backend using weston-launch */
	compositor->launcher = weston_launcher_connect(compositor, seat_id, true);
	if (compositor->launcher == NULL) {
		weston_log("fatal: drm backend should be run "
				"using weston-launch binary or as root\n");
		goto err_base;
	}

	if (init_drm_early(b) < 0) {
		weston_log("failed to initialize kms\n");
		goto err_launcher;
	}
	b->base.destroy = drm_destroy;
	b->base.repaint_begin = drm_repaint_begin;
	b->base.repaint_flush = drm_repaint_flush;
	b->base.repaint_cancel = drm_repaint_cancel;
	b->base.create_output = drm_output_create;

	/*
	* Initialize early temporary renderer which will be
	* used until backend is full ready
	*/
	if (init_early_renderer(b) < 0) {
		weston_log("failed to initialize early renderer\n");
		goto err_launcher;
	}

	GBM_PROTOCOL_LOG(LOG_DBG,"gbm_buf import=%p",
						   compositor->renderer->import_gbm_buffer);

	if (compositor->renderer->import_gbm_buffer) {
		if (gbm_buffer_backend_c_interface.setup(compositor) < 0)
			weston_log("Error: initializing gbm_buffer_backend_setup "
					"support failed.\n");
	}

	ret = weston_plugin_api_register(compositor, WESTON_DRM_OUTPUT_API_NAME,
					&api, sizeof(api));

	if (ret < 0) {
		weston_log("Failed to register output API.\n");
		goto err_sprite;
	}

	#ifdef ENABLE_EARLY_BOOT
	b->early_boot = true;
	weston_log("weston early boot enabled\n");
	#else
	b->early_boot = false;
	#endif

	b->first_repaint = true;

	struct full_init_param *full_init_param;
	full_init_param = (struct full_init_param *)zalloc(sizeof(struct full_init_param));
	if (!full_init_param) {
		weston_log("failed to create parameters for full init thread\n");
		goto err_sprite;
	}

	full_init_param->b = b;
	full_init_param->seat_id = seat_id;
	full_init_param->configure_device = config->configure_device;

	if (b->early_boot) {
		/*
		 * Initialize early drm display service which will be
		 * replaced by sdm service once backend gets full
		 * ready
		 */
		ret = early_drm_display_init(b->drm.fd);
		if (ret) {
			weston_log("failed to init drm display");
			goto err_sprite;
		}
		/* only create early display if early boot enable */
		if (drm_backend_create_heads(b) < 0) {
			weston_log("Failed to create heads for drm\n");
			goto err_display;
		}
		/*
		* If early boot is enabled, create another thread to do full backend
		* initialization.
		*/
		pthread_t full_init_tid;
		if (pthread_create(&full_init_tid, NULL,
				full_init_main, full_init_param)) {
			weston_log("failed to create full init thread\n");
			free(full_init_param);
			goto err_display;
		}
#if defined(ENABLE_RT_SCHEDULE)
		memset((char *)&params, 0x00, sizeof(struct sched_param));
		strlcpy(cpuConfigReq.procName, procName, strlen(procName) + 1);
		strlcpy(cpuConfigReq.thrdGrpName, thrdGrpName, strlen(thrdGrpName) + 1);

		CompResmgrRet_e ret = CompResmgrGetCPUConfig(&cpuConfigReq, &cpuConfigResp);

		if (COMPRESMGR_RET_SUCCESS == ret)
		{
			params.sched_priority = cpuConfigResp.priority;

			if (0 != pthread_setname_np(full_init_tid, cpuConfigReq.thrdGrpName)) {
				weston_log("pthread_setname_np: %s failed\n", cpuConfigReq.thrdGrpName);
			} else if (0 != pthread_setschedparam(full_init_tid, cpuConfigResp.schedPolicy, &params)) {
				weston_log("pthread_setschedparam: %s failed\n", cpuConfigReq.thrdGrpName);
			}
		} else {
			weston_log("CompResmgrGetCPUConfig: %s failed\n", cpuConfigReq.thrdGrpName);
		}
#endif
	} else {
		/* If early boot is disabled, do full backend initialization directly. */
		full_init_main(full_init_param);
		if (full_init_param->success) {
			free(full_init_param);
			return b;
		} else {
			free(full_init_param);
			wl_list_for_each_safe(base, next, &compositor->head_list, compositor_link) {
				drm_display_destroy(to_drm_head(base));
				drm_head_destroy(to_drm_head(base));
			}
			goto err_base;
		}
	}

	return b;

err_display:
	early_drm_display_deinit(true, b->drm.fd);
err_sprite:
	gbm_device_destroy(b->gbm);
err_launcher:
	weston_launcher_destroy(compositor->launcher);
err_base:
	weston_log_scope_destroy(b->debug);
	wl_list_remove(&b->base.link);
	free(b);
	return NULL;
}

static void
config_init_to_defaults(struct weston_drm_backend_config *config)
{
	config->use_pixman_shadow = true;
}

WL_EXPORT int
weston_backend_init(struct weston_compositor *compositor,
		struct weston_backend_config *config_base)
{
	struct drm_backend *b;
	struct weston_drm_backend_config config = {{ 0, }};
	int ret = -1;

	if (config_base == NULL ||
		config_base->struct_version != WESTON_DRM_BACKEND_CONFIG_VERSION ||
		config_base->struct_size > sizeof(struct weston_drm_backend_config)) {
		weston_log("drm backend config structure is invalid\n");
		return -1;
	}

	config_init_to_defaults(&config);
	memcpy(&config, config_base, config_base->struct_size);

	b = drm_backend_create(compositor, &config);
	if (b == NULL)
		return -1;

	ret = weston_compositor_load_gbm_buffer_backend();
	if (ret)
		weston_log("Failed to load gbm buffer backend\n");

	return ret;
}
