/*
* Copyright (c) 2017-2019, The Linux Foundation. All rights reserved.
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
*/
/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include <assert.h>
#include <stdarg.h>
#include <linux/limits.h>
#include <string>
#include <vector>
#include <map>
#include <ostream>
#include <fstream>
#include <utility>
#include <bitset>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include "sdm_display.h"
#include "uevent.h"

#ifdef __cplusplus
extern "C" {
#endif

#define __CLASS__ "SdmDisplay"


namespace sdm {
#define GET_GPU_TARGET_SLOT(max_layers) ((max_layers) - 1)
/* Cursor is fixed in (gpu_target_index-1) slot in SDM */
#define GET_CURSOR_SLOT(max_layers) ((max_layers) - 2)

#define SDM_DISPLAY_DEBUG 0
#define SDM_DISPLAY_DUMP_LAYER_STACK 0
extern struct gbm_buffer_backend_c_interface gbm_buffer_backend_c_interface;

enum {
  ORIGIN_TOP_LEFT, /* buffer content starts at (0,0) */
  ORIGIN_BOTTOM_LEFT, /* buffer content starts at (0, height) */
} buffer_origin;

Layer *SdmLayerManager::get_layer(struct sdm_layer *sdm_layer)
{
  std::lock_guard<std::mutex> lock(lock_);
  SdmLayer *layer;

  auto iter = layer_cache_.find(sdm_layer->view);
  if (iter == layer_cache_.end()) {
    layer = new SdmLayer;
    layer->layer_manager_ = this;
    layer->destroy_listener_.notify = destroy;
    layer_cache_.emplace(sdm_layer->view, layer);
    wl_signal_add(&sdm_layer->view->destroy_signal, &layer->destroy_listener_);
  } else {
    layer = iter->second;
  }

  return &layer->layer_;
}

void SdmLayerManager::destroy(struct wl_listener *listener, void *data)
{
  struct SdmLayer *layer = reinterpret_cast<struct SdmLayer *>(
      container_of(listener, struct SdmLayer, destroy_listener_));
  struct weston_view *view = reinterpret_cast<struct weston_view*>(data);

  std::lock_guard<std::mutex> lock(layer->layer_manager_->lock_);
  layer->layer_manager_->layer_cache_.erase(view);
  wl_list_remove(&layer->destroy_listener_.link);

  delete layer;
}

uint32_t SdmBufferManager::GetBufferId(int fd, struct weston_buffer *buffer)
{
  uint32_t buffer_id;

  std::lock_guard<std::mutex> lock(buffer_lock);
  auto iter = buffer_ids.find(fd);
  if (iter == buffer_ids.end()) {
    buffer_id = ++buffer_id_seed;
    buffer_ids.emplace(fd, buffer_id);
    if (buffer) {
      SdmBuffer *sdm_buf = new SdmBuffer;
      sdm_buf->destroy_listener.notify = destroy_notify;
      sdm_buf->buffer_manager = this;
      sdm_buf->fd = fd;
      wl_signal_add(&buffer->destroy_signal, &sdm_buf->destroy_listener);
    }
  } else {
    buffer_id = iter->second;
  }

  return buffer_id;
}

void SdmBufferManager::destroy_notify(struct wl_listener *listener, void *data)
{
  struct SdmBuffer *sdm_buf = reinterpret_cast<struct SdmBuffer *>(
    container_of(listener, struct SdmBuffer, destroy_listener));

  std::lock_guard<std::mutex> lock(sdm_buf->buffer_manager->buffer_lock);
  sdm_buf->buffer_manager->buffer_ids.erase(sdm_buf->fd);
  wl_list_remove(&sdm_buf->destroy_listener.link);

  delete sdm_buf;
}

int SdmDisplayInterface::GetDrmMasterFd() {
  DRMMaster *master = nullptr;
  int ret = DRMMaster::GetInstance(&master);
  int fd;

  master->GetHandle(&fd);
  return fd;
}

SdmDisplay::SdmDisplay(int32_t display_id, DisplayType type, CoreInterface *core_intf) {
  display_id_ = display_id;
  display_type_ = type;
  core_intf_    = core_intf;
  drm_head_   = NULL;
  drm_output_   = NULL;
  pageflip_cb_  = NULL;
}

SdmDisplay::~SdmDisplay() {
}

const char * SdmDisplay::FourccToString(uint32_t fourcc) {
  static __thread char s[5];
  uint32_t fmt = htole32(fourcc);

  memcpy(s, &fmt, 4);
  s[4] = '\0';

  return s;
}

DisplayError SdmDisplay::CreateDisplay() {
  DisplayError error = kErrorNone;
  struct DisplayHdrInfo display_hdr_info;

  error = core_intf_->CreateDisplay(display_id_, this, &display_intf_);

  if (error != kErrorNone) {
    DLOGE("Display creation failed. Error = %d", error);

    return error;
  }

  SdmDisplayDebugger::Get()->GetProperty("sys.sdm_display_disable_hdr", &disable_hdr_handling_);
  if (disable_hdr_handling_) {
    DLOGI("HDR Handling disabled");
  }

  GetHdrInfo(&display_hdr_info);

  if (hdr_supported_) {
    DLOGI("Display Device supports HDR functionality");
  } else {
    DLOGI("Display Device doesn't support HDR functionality");
  }

  return kErrorNone;
}

DisplayError SdmDisplay::DestroyDisplay() {
  DisplayError error;

  error = core_intf_->DestroyDisplay(display_intf_);
  display_intf_ = NULL;

  if (fb_layer_.buffer_map)
    fb_layer_.buffer_map->buffer_map.clear();

  return error;
}

DisplayError SdmDisplay::VSync(const DisplayEventVSync &vsync) {
  return kErrorNone;
}

DisplayError SdmDisplay::VSync(int fd, unsigned int sequence, unsigned int tv_sec,
                               unsigned int tv_usec, void *data) {
    DLOGW("Not implemented");

    return kErrorNone;
}

DisplayError SdmDisplay::PFlip(int fd, unsigned int sequence, unsigned int tv_sec,
                               unsigned int tv_usec, void *data) {
    pageflip_cb_(sequence, tv_sec, tv_usec, drm_output_);
    return kErrorNone;
}

DisplayError SdmDisplay::Refresh() {
  if (client_event_handler_) {
    client_event_handler_->Refresh();
  }

  return kErrorNone;
}

DisplayError SdmDisplay::CECMessage(char *message) {
  DLOGW("Not implemented");

  return kErrorNone;
}

DisplayError SdmDisplay::HandleEvent(DisplayEvent event) {
  switch (event) {
    case kIdleTimeout:
    case kThermalEvent:
    case kIdlePowerCollapse:
    case kPanelDeadEvent:
    case kDisplayPowerResetEvent:
    default:
      DLOGW("Not implemented for event: %d", event);
      break;
  }

  return kErrorNone;
}

DisplayError SdmDisplay::SetDisplayState(DisplayState state) {
  DisplayError error;
  bool teardown = false;

  if (state == kStateOff)
    teardown = true;

  error = display_intf_->SetDisplayState(state, teardown, nullptr);

  if (error != kErrorNone) {
    DLOGE("function failed. Error = %d", error);
  return error;
  }

  return kErrorNone;
}

DisplayError SdmDisplay::SetVSyncState(bool VSyncState, struct drm_output *output) {
  DisplayError error;

  if (drm_output_ && drm_output_ != output) {
    DLOGE("VSync state error: set different output for the same sdm display!");
    return kErrorNone;
  }

  if (!drm_output_)
    drm_output_ = output;

  error = display_intf_->SetVSyncState(VSyncState);
  if (error != kErrorNone) {
    DLOGE("VSync state setting failed. Error = %d", error);
    return error;
  }

  return kErrorNone;
}

DisplayError SdmDisplay::SetHead(struct drm_head *head) {
  DisplayError error;

  if (drm_head_ && drm_head_ != head) {
    DLOGE("set different head for the same sdm display!");
    return kErrorNone;
  }

  if (!drm_head_)
    drm_head_ = head;

  return kErrorNone;
}

DisplayError SdmDisplay::GetDisplayConfiguration(struct DisplayConfigInfo *display_config) {
  DisplayError error = kErrorNone;
  DisplayConfigVariableInfo disp_config;
  uint32_t active_index = 0;

  error = display_intf_->GetActiveConfig(&active_index);

  if (error != kErrorNone) {
    DLOGE("Active Index not found. Error = %d", error);
    return error;
  }

  error = display_intf_->GetConfig(active_index, &disp_config);

  if (error != kErrorNone) {
    DLOGE("Display Configuration failed. Error = %d", error);
    return error;
  }

  display_config->x_pixels     = disp_config.x_pixels;
  display_config->y_pixels     = disp_config.y_pixels;
  display_config->x_dpi        = disp_config.x_dpi;
  display_config->y_dpi        = disp_config.y_dpi;
  display_config->fps          = disp_config.fps;
  display_config->vsync_period_ns = disp_config.vsync_period_ns;
  display_config->is_yuv       = disp_config.is_yuv;
  fps_                         = disp_config.fps;

  return kErrorNone;
}

DisplayError SdmDisplay::RegisterCb(int display_id, pageflip_cb_t pflipcb) {
  DisplayError error = kErrorNone;

  pageflip_cb_ = pflipcb;
  display_id_  = display_id;

  return error;
}

DisplayError SdmDisplay::FreeLayerGeometry(struct LayerGeometry *glayer) {
  if (glayer->dirty_regions.count)
    free(glayer->dirty_regions.rects);
  if (glayer->visible_regions.count)
    free(glayer->visible_regions.rects);

  free(glayer);

  return kErrorNone;
}

static void SetRect(sdm::LayerRect *dst, struct Rect *src) {
  dst->left = src->left;
  dst->top = src->top;
  dst->right = src->right;
  dst->bottom = src->bottom;
}

static void SetRectArray(sdm::LayerRectArray *dst, struct RectArray *src) {
  for (uint32_t i = 0; i < src->count; i++)
    SetRect(&dst->rect[i], &src->rects[i]);
}

static uint32_t GetComposition(sdm::LayerComposition composition) {
  uint32_t ret;

  switch (composition) {
    case sdm::kCompositionGPUTarget:
      ret = SDM_COMPOSITION_FB_TARGET;
      break;
    case sdm::kCompositionGPU:
      ret = SDM_COMPOSITION_GPU;
      break;
    case sdm::kCompositionCursor:
      ret = SDM_COMPOSITION_HW_CURSOR;
      break;
    case sdm::kCompositionSDE:
      ret = SDM_COMPOSITION_OVERLAY;
      break;
    default:
      DLOGI("Unknown composition %d", (uint32_t)composition);
      ret = SDM_COMPOSITION_GPU;
      break;
  }

  return ret;
}

static bool NeedUpdateColorMetaData(struct LayerGeometry *layer_geometry) {
  bool need_update = false;

  if (layer_geometry->flags.hdr_present ||
    layer_geometry->flags.metadata_present)
    need_update = true;

  return need_update;
}

DisplayError SdmDisplay::PopulateLayerGeometryOnToLayerStack(struct drm_output *output,
                                                             uint32_t index,
                                                             struct LayerGeometry *glayer,
                                                             bool is_skip) {
  DisplayError error = kErrorNone;
  sdm::Layer *layer = layer_stack_.layers.at(index);
  struct LayerGeometry *layer_geometry = glayer;
  LayerBuffer *layer_buffer, &layer_buffer_ = layer->input_buffer;
  layer_buffer = &layer_buffer_;

  /* 1. Fill buffer information */
  *layer_buffer = sdm::LayerBuffer();
  layer_buffer->format = GetSDMFormat(layer_geometry->format, layer_geometry->flags);
  layer_buffer->width = layer_geometry->width;
  layer_buffer->height = layer_geometry->height;
  layer_buffer->unaligned_width = layer_geometry->unaligned_width;
  layer_buffer->unaligned_height = layer_geometry->unaligned_height;
  /* TODO (user): Obtain metadata and then */
  // TODO: (user)  if (SetMetaData(metadatar layer) != kErrorNone) {
  // TODO: (user)      return kErrorUndefined;
  // TODO: (user)  }

  layer_buffer->planes[0].fd = layer_geometry->ion_fd;
  layer_buffer->handle_id = layer_geometry->handle_id;

  /* TODO: Below information should be set according to the real user scenario */
  layer_buffer->flags.secure = layer_geometry->flags.secure_present;
  layer_buffer->flags.video = layer_geometry->flags.video_present;
  layer_buffer->flags.hdr = layer_geometry->flags.hdr_present;

  if (NeedUpdateColorMetaData(layer_geometry)) {
    layer_buffer->color_metadata = layer_geometry->color_metadata;
  }

  layer_buffer->flags.macro_tile = false;
  layer_buffer->flags.interlace = false;
  layer_buffer->flags.secure_display = false;
  layer_buffer->flags.secure_camera = false;
  /* 2. Fill layer information */
  if (layer_geometry->composition == SDM_COMPOSITION_FB_TARGET)
    layer->composition = sdm::kCompositionGPUTarget;
  else
    layer->composition = sdm::kCompositionGPU;

  SetRect(&layer->src_rect, &layer_geometry->src_rect);
  SetRect(&layer->dst_rect, &layer_geometry->dst_rect);

  for (uint32_t i = 0; i < layer_geometry->visible_regions.count; i++) {
    SetRect(&layer->visible_regions.at(i), &layer_geometry->visible_regions.rects[i]);
  }

  for (uint32_t i = 0; i < layer_geometry->dirty_regions.count; i++) {
    SetRect(&layer->dirty_regions.at(i), &layer_geometry->dirty_regions.rects[i]);
  }

  layer->blending = GetSDMBlending(layer_geometry->blending);
  layer->plane_alpha = layer_geometry->plane_alpha;

  layer->transform.flip_horizontal =
      ((layer_geometry->transform & SDM_TRANSFORM_FLIP_H) > 0);
  layer->transform.flip_vertical =
      ((layer_geometry->transform & SDM_TRANSFORM_FLIP_V) > 0);
  layer->transform.rotation =
      ((layer_geometry->transform & SDM_TRANSFORM_90) ? 90.0f : 0.0f);

  layer->frame_rate = fps_;
  layer->solid_fill_color = 0;

  layer->flags.skip = is_skip;
  if (is_skip)
    layer_stack_.flags.skip_present = is_skip;
  if (layer_buffer->flags.video)
    layer_stack_.flags.video_present = true;
  if (layer_buffer->flags.hdr)
    layer_stack_.flags.hdr_present = 1;
  if (layer_buffer->flags.secure)
    layer_stack_.flags.secure_present = true;
  layer->flags.updating = true;
  layer->flags.solid_fill = false;
  layer->flags.cursor = false;
  layer->flags.single_buffer = false;

  return error;
}

DisplayError SdmDisplay::AllocateMemoryForLayerGeometry(struct drm_output *output,
                                                        uint32_t index,
                                                        struct LayerGeometry *glayer) {
  /* Configure each layer */
  uint32_t num_visible_rects = 0;
  uint32_t num_dirty_rects = 0;

  if (index < output->view_count) {
    if (glayer == NULL) {
      DLOGE("no layer geometry for the display!\n");
      return kErrorParameters;
    }
    /* It's permissive the visible/dirty region can be NULL */
    layer_stack_.layers.at(index)->visible_regions.clear();
    layer_stack_.layers.at(index)->dirty_regions.clear();

    num_visible_rects = glayer->visible_regions.count;
    for (uint32_t j = 0; j < num_visible_rects; j++) {
      LayerRect visible_rect {};
      layer_stack_.layers.at(index)->visible_regions.push_back(visible_rect);
    }

    num_dirty_rects = glayer->dirty_regions.count;
    for (uint32_t j = 0; j < num_dirty_rects; j++) {
      LayerRect dirty_rect {};
      layer_stack_.layers.at(index)->dirty_regions.push_back(dirty_rect);
    }
  }

  return kErrorNone;
}

DisplayError SdmDisplay::AddGeometryLayerToLayerStack(struct drm_output *output, uint32_t index,
                                                      struct LayerGeometry *glayer, bool is_skip) {
  DisplayError error = kErrorNone;

  AllocateMemoryForLayerGeometry(output, index, glayer);
  error = PopulateLayerGeometryOnToLayerStack(output, index, glayer, is_skip);

  return error;
}

int SdmDisplay::PrepareFbLayerGeometry(struct drm_output *output,
                                       struct LayerGeometry **fb_glayer) {
  struct LayerGeometry *fb_layer;

  *fb_glayer = fb_layer = reinterpret_cast<struct LayerGeometry *> \
              (zalloc(sizeof *fb_layer));
  if (!fb_layer) {
    DLOGE("out of memory for allocating fb layer\n");
    return -1;
  }

  fb_layer->width = output->base.current_mode->width;
  fb_layer->height = output->base.current_mode->height;
  fb_layer->unaligned_width = output->base.current_mode->width;
  fb_layer->unaligned_height = output->base.current_mode->height;

  fb_layer->format = GetMappedFormatFromGbm(output->format->format);
  fb_layer->composition = SDM_COMPOSITION_FB_TARGET;

  fb_layer->src_rect.left = (float)0.0;
  fb_layer->src_rect.top = (float)0.0;
  fb_layer->src_rect.right = (float)output->base.current_mode->width;
  fb_layer->src_rect.bottom = (float)output->base.current_mode->height;
  fb_layer->dst_rect.left = (float)0.0;
  fb_layer->dst_rect.top = (float)0.0;
  fb_layer->dst_rect.right = (float)output->base.current_mode->width;
  fb_layer->dst_rect.bottom = (float)output->base.current_mode->height;

  fb_layer->visible_regions.rects = reinterpret_cast<struct Rect *> \
                    (zalloc(sizeof(struct Rect)));
  if (fb_layer->visible_regions.rects == NULL) {
    DLOGE("out of memory for allocating fb visible region\n");
    return -1;
  }
  fb_layer->visible_regions.rects[0] = fb_layer->dst_rect;
  fb_layer->visible_regions.count = 1;

  fb_layer->dirty_regions.rects = reinterpret_cast<struct Rect *> \
                  (zalloc(sizeof(struct Rect)));
  if (fb_layer->dirty_regions.rects == NULL) {
    DLOGE("out of memory for allocating fb dirty region\n");
    return -1;
  }
  fb_layer->dirty_regions.rects[0] = fb_layer->dst_rect;
  fb_layer->dirty_regions.count = 1;

  fb_layer->blending = SDM_BLENDING_PREMULTIPLIED;
  /*
   * output->base.width/height should be already transformed, so just
   * keep no transform for output.
   */
  fb_layer->transform = SDM_TRANSFORM_NORMAL;
  fb_layer->plane_alpha = 0xFF;

  fb_layer->flags.skip = 0;
  fb_layer->flags.is_cursor = 0;
  fb_layer->flags.has_ubwc_buf = output->framebuffer_ubwc;
  fb_layer->flags.secure_present = output->is_secure;
  /* set previous frame's fd for Validate() */
  if (output->current) {
    fb_layer->ion_fd = output->current->ion_fd;
    fb_layer->handle_id = buffer_manager_.GetBufferId(fb_layer->ion_fd, NULL);
  } else {
    fb_layer->ion_fd = -1;
  }

  return 0;
}

static bool SetCSC(int32_t color_space, ColorMetaData *color_metadata) {
  bool csc_updated = false;
  /*
   * As any GBM color space definition can't be 0, if color_space is still 0
   * here, which means nobody touched color space or meta data info before, so
   * we should skip the following update.
   */
  if (!color_metadata || !color_space)
    return csc_updated;

  /*
   * A tricky design in gbm is, the color space will be updated according to
   * the color_primaries and range setting in meta data info if
   * GBM_METADATA_SET_COLOR_METADATA was ever called before while calling
   * GBM_METADATA_GET_COLOR_SPACE. In this case, we just update those two fields
   * of meta data.
   */
  if (color_space == GBM_METADATA_COLOR_SPACE_ITU_R_601_FR ||
      color_space == GBM_METADATA_COLOR_SPACE_ITU_R_2020_FR)
    color_metadata->range = Range_Full;

  switch (color_space) {
    case GBM_METADATA_COLOR_SPACE_ITU_R_601:
    case GBM_METADATA_COLOR_SPACE_ITU_R_601_FR:
      color_metadata->colorPrimaries = ColorPrimaries_BT601_6_625;
      color_metadata->matrixCoefficients = MatrixCoEff_BT601_6_625;
      csc_updated = true;
      break;
    case GBM_METADATA_COLOR_SPACE_ITU_R_709:
      color_metadata->colorPrimaries = ColorPrimaries_BT709_5;
      color_metadata->matrixCoefficients = MatrixCoEff_BT709_5;
      csc_updated = true;
      break;
    case GBM_METADATA_COLOR_SPACE_ITU_R_2020:
    case GBM_METADATA_COLOR_SPACE_ITU_R_2020_FR:
      color_metadata->colorPrimaries = ColorPrimaries_BT2020;
      color_metadata->matrixCoefficients = MatrixCoEff_BT2020;
      csc_updated = true;
      break;
    default:
      DLOGE("unsupported CSC: %d", color_space);
      break;
  }

  return csc_updated;
}

int SdmDisplay::PrepareNormalLayerGeometry(struct drm_output *output,
                                           struct LayerGeometry **glayer,
                                           struct sdm_layer *sdm_layer) {
  struct drm_backend *b = (struct drm_backend *)output->base.backend;
  struct LayerGeometry *layer;
  struct weston_view *ev = sdm_layer->view;
  struct weston_paint_node *pnode = sdm_layer->pnode;
  struct weston_surface *es = ev->surface;
  struct weston_compositor *ec = es->compositor;
  bool is_cursor = sdm_layer->is_cursor;
  uint32_t format = GBM_FORMAT_XBGR8888;
  struct linux_dmabuf_buffer *dmabuf;
  struct gbm_buffer *gbm_buf;
  pixman_region32_t r;
  int bo_fd = -1;

  *glayer = layer = reinterpret_cast<struct LayerGeometry *> \
                    (zalloc(sizeof *layer));
  if (!layer) {
    DLOGE("out of memory for allocating layer\n");
    return -1;
  }

  /* Prepare layer buffer information */
  layer->width = es->width;
  layer->height = es->height;
  layer->fb_id = -1;
  layer->format = SDM_BUFFER_FORMAT_RGBX_8888;
  layer->transform = SDM_TRANSFORM_NORMAL;

  if (!sdm_layer->is_skip) {
    struct gbm_bo *bo = NULL;
    //check whether the buffer resource is created by linux dma buf
    if ((dmabuf = linux_dmabuf_buffer_get(ec, es->buffer_ref.buffer->resource))) {
      struct dmabuf_attributes *attributes = &dmabuf->attributes;
      if (attributes->modifier[0] == DRM_FORMAT_MOD_INVALID) {
        struct gbm_import_fd_data gbm_dmabuf = {
          .fd     = attributes->fd[0],
          .width  = attributes->width,
          .height = attributes->height,
          .stride = attributes->stride[0],
          .format = attributes->format
        };
        bo = gbm_bo_import(b->gbm, GBM_BO_IMPORT_FD, &gbm_dmabuf, GBM_BO_USE_SCANOUT);
      } else {
        struct gbm_import_fd_modifier_data gbm_dmabuf = {
          .width   = attributes->width,
          .height  = attributes->height,
          .format  = attributes->format,
          .num_fds = 1,
          .modifier = attributes->modifier[0]
        };
        gbm_dmabuf.fds[0] = attributes->fd[0];
        bo = gbm_bo_import(b->gbm, GBM_BO_IMPORT_FD_MODIFIER, &gbm_dmabuf, GBM_BO_USE_SCANOUT);
      }
      bo_fd = attributes->fd[0];
      /*
       * Let the layer V-flipped if the weston_buffer is NOT y_inverted, to
       * make layer orientation the same between overlay and gpu composition path.
       */
      if (es->buffer_ref.buffer->buffer_origin == ORIGIN_BOTTOM_LEFT)
        layer->transform = SDM_TRANSFORM_FLIP_V;
    } else if ((gbm_buf = gbm_buffer_backend_c_interface.buffer_get(es->buffer_ref.buffer->resource))) {
      struct gbm_buf_info gbm_bufinfo = {
        .fd           = gbm_buf->fd,
        .metadata_fd  = gbm_buf->metadata_fd,
        .width        = gbm_buf->width,
        .height       = gbm_buf->height,
        .format       = gbm_buf->format
      };
      bo = gbm_bo_import(b->gbm, GBM_BO_IMPORT_GBM_BUF_TYPE,
                         &gbm_bufinfo,
                         GBM_BO_USE_SCANOUT);
      bo_fd = gbm_buf->fd;

      if (es->buffer_ref.buffer->buffer_origin == ORIGIN_BOTTOM_LEFT)
        layer->transform = SDM_TRANSFORM_FLIP_V;
    } else {
      /* Assume the unknown buffer resource as gbm buffer to get the fd value */
      void* temp_buf = wl_resource_get_user_data(es->buffer_ref.buffer->resource);
      bo = gbm_bo_import(b->gbm, GBM_BO_IMPORT_GBM_BUF_TYPE,
                         temp_buf,
                         GBM_BO_USE_SCANOUT);
      struct gbm_buf_info *temp_gbm_buf = reinterpret_cast<struct gbm_buf_info*>(temp_buf);
      if (temp_gbm_buf)
        bo_fd = temp_gbm_buf->fd;
    }

    if (bo == NULL)
      DLOGE("fail to import gbm bo!\n");
    else {
      uint32_t width, height;
      uint32_t *fbid;
      uint32_t fb_id, stride, handle, size;
      uint32_t fb_id1;
      int ret;

      //save gbm bo in sdm layer for future reference.
      sdm_layer->bo = bo;

      width = gbm_bo_get_width(bo);
      height = gbm_bo_get_height(bo);
      stride = gbm_bo_get_stride(bo);
      handle = gbm_bo_get_handle(bo).u32;
      format = gbm_bo_get_format(bo);
      int drm_fd = SdmDisplayInterface::GetDrmMasterFd();

      uint32_t handles[4], pitches[4], offsets[4];
      handles[0] = handle;
      pitches[0] = stride;
      offsets[0] = 0;

      uint32_t alignedWidth = 0;
      uint32_t alignedHeight = 0;
      uint32_t secure_status = 0;
      uint32_t ubwc_status = 0;
      int32_t color_space = 0;
      bool metadata_present = false;
      void *prm = reinterpret_cast<void *> (&layer->color_metadata);

      gbm_perform(GBM_PERFORM_GET_BO_ALIGNED_WIDTH, bo, &alignedWidth);
      gbm_perform(GBM_PERFORM_GET_BO_ALIGNED_HEIGHT, bo, &alignedHeight);
      gbm_perform(GBM_PERFORM_GET_SECURE_BUFFER_STATUS, bo, &secure_status);
      ret = gbm_perform(GBM_PERFORM_GET_METADATA, bo, GBM_METADATA_GET_COLOR_METADATA, prm);
      if (ret == GBM_ERROR_NONE)
        metadata_present = true;
      /* Only query color space info when color meta data never be set before */
      if (!metadata_present) {
        gbm_perform(GBM_PERFORM_GET_METADATA, bo, GBM_METADATA_GET_COLOR_SPACE, &color_space);
      }
      gbm_perform(GBM_PERFORM_GET_UBWC_STATUS, bo, &ubwc_status);

      /* Override buffer width/height to reflect aligned width and aligned height */
      layer->width = alignedWidth;
      layer->height = alignedHeight;
      layer->unaligned_width = width;
      layer->unaligned_height = height;
      /*
       * Since layer->ion_fd will be used as key of buffer map, avoid duplicate fd by
       * gbm_bo_get_fd() here. Instead, reserve the fd at bo creation and assign it
       * to layer->ion_fd.
       */
      layer->ion_fd = bo_fd;
      layer->flags.secure_present = secure_status;
      layer->flags.has_ubwc_buf = ubwc_status;
      layer->handle_id = buffer_manager_.GetBufferId(layer->ion_fd, sdm_layer->buffer_ref.buffer);

      /* Update metadata info according to color space setting in gbm */
      if (!metadata_present)
        metadata_present = SetCSC(color_space, &layer->color_metadata);
      layer->flags.metadata_present = metadata_present;

      bool hdr_layer = layer->color_metadata.colorPrimaries == ColorPrimaries_BT2020 &&
                       (layer->color_metadata.transfer == Transfer_SMPTE_ST2084 ||
                       layer->color_metadata.transfer == Transfer_HLG);

      // Set to true if incoming layer has HDR support and Display supports HDR functionality
      layer->flags.hdr_present = hdr_layer && hdr_supported_;
    }
  }

  if (NeedConvertGbmFormat(ev, format))
    format = ConvertToOpaqueGbmFormat(format);
  layer->format = GetMappedFormatFromGbm(format);

  /* Get src/dst rect */
  ComputeSrcDstRect(output, pnode, &layer->src_rect, &layer->dst_rect);
  /* Get visible rects */
  if (GetVisibleRegion(output, ev, &sdm_layer->overlap, &layer->visible_regions))
    return -1;
  /*
   * Get dirty rects. It's not surprised a view has null damage region, that means
   * the buffer content of view is not changed since last frame, eg. cursor.
   */
  if (ComputeDirtyRegion(ev, &layer->dirty_regions))
    return -1;

  /* Get global alpha */
  layer->plane_alpha = GetGlobalAlpha(ev);

  /* TODO: update property src_config, csc, color_fill, scaler ... */
  layer->flags.is_cursor = is_cursor;
  layer->flags.video_present = GetVideoPresenceByFormatFromGbm(format);

  /* compute whether this view has no blending */
  pixman_region32_init_rect(&r, 0, 0, ev->surface->width, ev->surface->height);
  pixman_region32_subtract(&r, &r, &ev->surface->opaque);

  if ((!pixman_region32_not_empty(&r) || layer->flags.video_present)
      && (layer->plane_alpha == 0xFF))
    layer->blending = SDM_BLENDING_NONE;
  else
    layer->blending = SDM_BLENDING_PREMULTIPLIED;
  pixman_region32_fini(&r);

  /* Initialize all views with GPU composition first, SDM will update them after prepare */
  layer->composition = SDM_COMPOSITION_GPU;

  return 0;
}

DisplayError SdmDisplay::PrePrepareLayerStack(struct drm_output *output) {
  DisplayError error = kErrorNone;
  struct sdm_layer *sdm_layer = NULL, *next_sdm_layer = NULL;
  struct LayerGeometry *glayer = NULL;
  uint32_t gpu_target_index = GET_GPU_TARGET_SLOT(output->view_count);
  uint32_t index = 0;

  if (shutdown_pending_) {
    return kErrorShutDown;
  }

  layer_stack_ = {};

#if SDM_DISPLAY_DEBUG
  DLOGW("gpu_target_index = %d\n", gpu_target_index);
#endif

  /* If no view can be handled by SDM, just skip below and prepare fb target directly. */
  if (gpu_target_index > 0) {
    wl_list_for_each_reverse(sdm_layer, &output->sdm_layer_list, link) {
      layer_stack_.layers.push_back(layer_manager_.get_layer(sdm_layer));

      glayer = NULL;
      if(PrepareNormalLayerGeometry(output, &glayer, sdm_layer)) {
        DLOGE("fail to prepare normal layer geometry.");
        return kErrorUndefined;
      }

      error = AddGeometryLayerToLayerStack(output, index++, glayer, sdm_layer->is_skip);
      if (error) {
        DLOGE("failed add Geometry Layer to LayerStack.");
        FreeLayerGeometry(glayer);
        return kErrorUndefined;
      }
      FreeLayerGeometry(glayer);

      if (sdm_layer->is_skip)
        layer_stack_.flags.skip_present = true;
    }
  }

  layer_stack_.layers.push_back(&fb_layer_);
  int err = PrepareFbLayerGeometry(output, &glayer);
  if (err) {
    DLOGE("failed to prepare Layer Geometry Fb target\n");
    return kErrorUndefined;
  } else {
    error = AddGeometryLayerToLayerStack(output, index, glayer, false);
    if (error) {
      DLOGE("fail to prepare Fb target: Add Geometry failure fb\n");
      FreeLayerGeometry(glayer);
      return kErrorUndefined;
    }
    FreeLayerGeometry(glayer);
  }

  return error;
}

DisplayError SdmDisplay::PrePrepare(struct drm_output *output) {
  DisplayError error = kErrorNone;

  error = PrePrepareLayerStack(output);
  if (error ) {
    DLOGE("function failed!\n", error);
  }

  return error;
}

DisplayError SdmDisplay::PostPrepare(struct drm_output *output) {
  DisplayError error = kErrorNone;
  int index = 0;
  struct sdm_layer *sdm_layer = NULL;

  wl_list_for_each_reverse(sdm_layer, &output->sdm_layer_list, link) {
    Layer *layer = layer_stack_.layers.at(index);

    if (layer->composition == kCompositionGPU) {
      sdm_layer->composition_type = SDM_COMPOSITION_GPU;
    } else {
      sdm_layer->composition_type = SDM_COMPOSITION_OVERLAY;
    }
    index++;
  }

  return error;
}

static void GetLayerStackDump(void *layerStack, char *buffer, uint32_t length) {
  if (!buffer || !length) {
    return;
  }
  static int frame_count=0;
  buffer[0] = '\0';
  struct LayerStack *layer_stack;
  layer_stack = reinterpret_cast<struct LayerStack *>(layerStack);
  DLOGI("\n-------- Display Manager: Layer Stack Dump --------");
  fprintf(stderr,"Frame:%d LayerStack: NumLayers:%d flags:0x%x\n",
          frame_count, layer_stack->layers.size(), layer_stack->flags);

  for (uint32_t i = 0; i < layer_stack->layers.size(); i++) {

    struct Layer *layer;
    struct LayerBuffer buffer;
    layer = layer_stack->layers.at(i);
    buffer = layer->input_buffer;

    fprintf(stderr,"\nLayer: %d\n    width  = %d,     height = %d", i,
            layer->input_buffer.width, layer->input_buffer.height);
    fprintf(stderr,"\n LayerComposition = %#x", layer->composition);
    fprintf(stderr,"\n src_rect (LTRB) = %4.2f, %4.2f, %4.2f, %4.2f",
            layer->src_rect.left, layer->src_rect.top,
            layer->src_rect.right, layer->src_rect.bottom);
    fprintf(stderr,"\n dst_rect (LTRB) = %4.2f, %4.2f, %4.2f, %4.2f",
            layer->dst_rect.left, layer->dst_rect.top, layer->dst_rect.right,
            layer->dst_rect.bottom);
    fprintf(stderr,"\n LayerBlending = %#x", layer->blending);
    fprintf(stderr,"\n LayerTransform:rotation= %f,flip_horizontal=%s,flip_vertical=%s",
            layer->transform.rotation, (layer->transform.flip_horizontal? \
            "true":"false"), (layer->transform.flip_vertical? "true":"false"));
    fprintf(stderr,"\n Plane Alpha = %#x, frame_rate = %d,  solid_fill_color = %d",
            layer->plane_alpha, layer->frame_rate, layer->solid_fill_color);
    fprintf(stderr,"\n LayerFlags = %#x", layer->flags);
    fprintf(stderr,"\t LayerFlags.skip = %d", layer->flags.skip);
    fprintf(stderr,"\n LayerBuffer Flags: hdr:%d secure:%d video:%d",
            buffer.flags.hdr, buffer.flags.secure, buffer.flags.video);
    fprintf(stderr,"\n");
  }
  frame_count++;

  return;
}

DisplayError SdmDisplay::Prepare(struct drm_output *output) {
  DisplayError error = kErrorNone;
  char dump_buffer[8192] = {0};

  error = PrePrepare(output);

#if SDM_DISPLAY_DUMP_LAYER_STACK
  // Dump all input layers of the layer stack:
  GetLayerStackDump(&layer_stack_, dump_buffer, sizeof(dump_buffer));
#endif

  error = display_intf_->Prepare(&layer_stack_);
  output->prev_layer_none_commit = output->layer_none_commit;
  if (error == kErrorNoAppLayers)
    output->layer_none_commit = true;
  else
    output->layer_none_commit = false;
  //DumpInterface::GetDump(dump_buffer, sizeof(dump_buffer));
  error = PostPrepare(output);
  if (error != kErrorNone)
    DLOGE("function failed Error= %d\n", error);

  return error;
}

DisplayError SdmDisplay::PreCommit() {
  DisplayError error = kErrorNone;

  return error;
}

DisplayError SdmDisplay::PostCommit(int *retire_fence_fd) {
  DisplayError error = kErrorNone;

  //Iterate through the layer buffer and close release fences
  for (uint32_t i = 0; i < layer_stack_.layers.size(); i++) {
    Layer *layer = layer_stack_.layers.at(i);
    LayerBuffer *layer_buffer = &layer->input_buffer;

    if (layer_buffer->release_fence_fd > 0) {
      close(layer_buffer->release_fence_fd);
      layer_buffer->release_fence_fd = -1;
    }
  }

  *retire_fence_fd = layer_stack_.retire_fence_fd;
  layer_stack_.retire_fence_fd = -1;

  return error;
}

DisplayError SdmDisplay::Commit(struct drm_output *output) {
  DisplayError ret = kErrorNone;

  uint32_t layer_count = layer_stack_.layers.size();

  uint32_t GPUTarget_index = layer_count-1;
  Layer *GpuTargetlayer;

  GpuTargetlayer = layer_stack_.layers.at(GPUTarget_index);

  /* if no need gpu composition, output->next could be NULL*/
  if (output->next) {
    GpuTargetlayer->input_buffer.planes[0].fd = output->next->ion_fd;
    GpuTargetlayer->input_buffer.handle_id =
            buffer_manager_.GetBufferId(output->next->ion_fd, NULL);
  }

  PreCommit();

  ret = display_intf_->Commit(&layer_stack_);
  PostCommit(&output->retire_fence_fd);

  return ret;
}

DisplayError SdmDisplay::Flush() {
  DisplayError ret = kErrorNone;

  ret = display_intf_->Flush(&layer_stack_);

  return ret;
}

/* Adding following  support functions */
LayerBufferFormat SdmDisplay::GetSDMFormat(uint32_t src_fmt, struct LayerGeometryFlags flags) {
  LayerBufferFormat format = kFormatInvalid;

  if (flags.has_ubwc_buf) {
    switch (src_fmt) {
      case SDM_BUFFER_FORMAT_RGBA_8888:
        format = sdm::kFormatRGBA8888Ubwc;
        break;
      case SDM_BUFFER_FORMAT_RGBX_8888:
        format = sdm::kFormatRGBX8888Ubwc;
        break;
      case SDM_BUFFER_FORMAT_RGB_565:
        format = sdm::kFormatBGR565Ubwc;
        break;
      case SDM_BUFFER_FORMAT_RGBA_2101010:
        format = sdm::kFormatRGBA1010102Ubwc;
        break;
      case SDM_BUFFER_FORMAT_YCbCr_420_SP_VENUS:
      case SDM_BUFFER_FORMAT_NV12_ENCODEABLE:
        format = sdm::kFormatYCbCr420SPVenusUbwc;
        break;
      case SDM_BUFFER_FORMAT_YCbCr_420_TP10_UBWC:
        format = sdm::kFormatYCbCr420TP10Ubwc;
        break;
      case SDM_BUFFER_FORMAT_YCbCr_420_P010_UBWC:
        format = sdm::kFormatYCbCr420P010Ubwc;
        break;
      default:
        DLOGE("Unsupported UBWC format %d\n", src_fmt);
        return sdm::kFormatInvalid;
    }

    return format;
  }

  switch (src_fmt) {
    case SDM_BUFFER_FORMAT_RGBA_8888:
      format = sdm::kFormatRGBA8888;
      break;
    case SDM_BUFFER_FORMAT_ARGB_8888:
      format = sdm::kFormatARGB8888;
      break;
//  case SDM_BUFFER_FORMAT_ABGR_8888:
//    format = sdm::kFormatABGR8888;
      break;
    case SDM_BUFFER_FORMAT_RGBA_5551:
      format = sdm::kFormatRGBA5551;
      break;
    case SDM_BUFFER_FORMAT_RGBA_4444:
      format = sdm::kFormatRGBA4444;
      break;
    case SDM_BUFFER_FORMAT_BGRA_8888:
      format = sdm::kFormatBGRA8888;
      break;
    case SDM_BUFFER_FORMAT_RGBX_8888:
      format = sdm::kFormatRGBX8888;
      break;
    case SDM_BUFFER_FORMAT_BGRX_8888:
      format = sdm::kFormatBGRX8888;
      break;
    case SDM_BUFFER_FORMAT_RGB_888:
      format = sdm::kFormatRGB888;
      break;
    case SDM_BUFFER_FORMAT_BGR_888:
      format = sdm::kFormatBGR888;
      break;
    case SDM_BUFFER_FORMAT_RGB_565:
      format = sdm::kFormatRGB565;
      break;
    case SDM_BUFFER_FORMAT_BGR_565:
      format = sdm::kFormatBGR565;
      break;
    case SDM_BUFFER_FORMAT_NV12_ENCODEABLE:
    case SDM_BUFFER_FORMAT_YCbCr_420_SP_VENUS:
      format = sdm::kFormatYCbCr420SemiPlanarVenus;
      break;
    case SDM_BUFFER_FORMAT_ABGR_2101010:
      format = sdm::kFormatABGR2101010;
      break;
    case SDM_BUFFER_FORMAT_RGBA_2101010:
      format = sdm::kFormatRGBA1010102;
      break;
    case SDM_BUFFER_FORMAT_YCbCr_420_TP10_UBWC:
      format = sdm::kFormatYCbCr420TP10Ubwc;
      break;
    case SDM_BUFFER_FORMAT_YCbCr_420_P010_UBWC:
      format = sdm::kFormatYCbCr420P010Ubwc;
      break;
    case SDM_BUFFER_FORMAT_YV12:
      format = sdm::kFormatYCrCb420PlanarStride16;
      break;
    case SDM_BUFFER_FORMAT_YCbCr_420_P:
      format = sdm::kFormatYCbCr420Planar;
      break;
    case SDM_BUFFER_FORMAT_YCrCb_420_P:
      format = sdm::kFormatYCrCb420Planar;
      break;
    case SDM_BUFFER_FORMAT_YCrCb_420_SP:
      format = sdm::kFormatYCrCb420SemiPlanar;
      break;
    case SDM_BUFFER_FORMAT_YCbCr_420_SP:
      format = sdm::kFormatYCbCr420SemiPlanar;
      break;
    case SDM_BUFFER_FORMAT_YCbCr_422_SP:
      format = sdm::kFormatYCbCr422H2V1SemiPlanar;
      break;
    case SDM_BUFFER_FORMAT_YCbCr_422_I:
      format = sdm::kFormatYCbCr422H2V1Packed;
      break;
    case SDM_BUFFER_FORMAT_CbYCrY_422_I:
      format = sdm::kFormatCbYCrY422H2V1Packed;
      break;
    case SDM_BUFFER_FORMAT_YCbCr_420_P010_VENUS:
      format = sdm::kFormatYCbCr420P010Venus;
      break;
    case SDM_BUFFER_FORMAT_P010:
      format = sdm::kFormatYCbCr420P010;
      break;
//  case SDM_BUFFER_FORMAT_CrYCbY_422_I:
//    format = sdm::kFormatCrYCbY422H2V1Packed;
//    break;
//  case SDM_BUFFER_FORMAT_YCbYCr_422_I:
//    format = sdm::kFormatYCbYCr422H2V1Packed;
//    break;
//  case SDM_BUFFER_FORMAT_YCrYCb_422_I:
//    format = sdm::kFormatYCrYCb422H2V1Packed;
//    break;
    default:
      DLOGE("Unsupported format %d\n", src_fmt);
      return sdm::kFormatInvalid;
  }

  return format;
}

LayerBlending SdmDisplay::GetSDMBlending(uint32_t source) {
  sdm::LayerBlending blending = sdm::kBlendingPremultiplied;

  switch (source) {
    case SDM_BLENDING_PREMULTIPLIED:
      blending = sdm::kBlendingPremultiplied;
      break;
    case SDM_BLENDING_NONE:
    default:
      blending = sdm::kBlendingOpaque;
      break;
  }

  return blending;
}

bool SdmDisplay::GetVideoPresenceByFormatFromGbm(uint32_t fmt) {
  bool is_video_present = false;

  switch (fmt) {
    case GBM_FORMAT_NV12:
    case GBM_FORMAT_UYVY:
    case GBM_FORMAT_VYUY:
    case GBM_FORMAT_YUYV:
    case GBM_FORMAT_YVYU:
    case GBM_FORMAT_YCbCr_420_TP10_UBWC:
    case GBM_FORMAT_YCbCr_420_P010_UBWC:
      is_video_present = true;
      break;
    default:
      is_video_present = false;
      break;
  }

  return is_video_present;
}

uint32_t SdmDisplay::GetMappedFormatFromGbm(uint32_t fmt) {
  uint32_t ret = SDM_BUFFER_FORMAT_INVALID;

  switch (fmt) {
    case GBM_FORMAT_ARGB8888:
      ret = SDM_BUFFER_FORMAT_BGRA_8888;
      break;
    case GBM_FORMAT_XRGB8888:
      ret = SDM_BUFFER_FORMAT_BGRX_8888;
      break;
    case GBM_FORMAT_ABGR8888:
      ret = SDM_BUFFER_FORMAT_RGBA_8888;
      break;
    case GBM_FORMAT_XBGR8888:
      ret = SDM_BUFFER_FORMAT_RGBX_8888;
      break;
    case GBM_FORMAT_BGRA8888:
      ret = SDM_BUFFER_FORMAT_ARGB_8888;
      break;
    case GBM_FORMAT_RGBA8888:
      ret = SDM_BUFFER_FORMAT_ABGR_8888;
      break;
    case GBM_FORMAT_ARGB4444:
    case GBM_FORMAT_ABGR4444:
      ret = SDM_BUFFER_FORMAT_RGBA_4444;
      break;
    case GBM_FORMAT_ARGB1555:
    case GBM_FORMAT_ABGR1555:
      ret = SDM_BUFFER_FORMAT_RGBA_5551;
      break;
    case GBM_FORMAT_RGB565:
      ret = SDM_BUFFER_FORMAT_BGR_565;
      break;
    case GBM_FORMAT_BGR565:
      ret = SDM_BUFFER_FORMAT_RGB_565;
      break;
    case GBM_FORMAT_RGB888:
      ret = SDM_BUFFER_FORMAT_BGR_888;
      break;
    case GBM_FORMAT_BGR888:
      ret = SDM_BUFFER_FORMAT_RGB_888;
      break;
    case GBM_FORMAT_YUV420:
      ret = SDM_BUFFER_FORMAT_YCbCr_420_P;
      break;
    case GBM_FORMAT_YVU420:
      ret = SDM_BUFFER_FORMAT_YCrCb_420_P;
      break;
    case GBM_FORMAT_NV12:
      ret = SDM_BUFFER_FORMAT_YCbCr_420_SP_VENUS;
      break;
    case GBM_FORMAT_UYVY:
      ret = SDM_BUFFER_FORMAT_CbYCrY_422_I;
      break;
    case GBM_FORMAT_VYUY:
      ret = SDM_BUFFER_FORMAT_CrYCbY_422_I;
      break;
    case GBM_FORMAT_YUYV:
      ret = SDM_BUFFER_FORMAT_YCbYCr_422_I;
      break;
    case GBM_FORMAT_YVYU:
      ret = SDM_BUFFER_FORMAT_YCrYCb_422_I;
      break;
    case GBM_FORMAT_ABGR2101010:
      ret = SDM_BUFFER_FORMAT_RGBA_2101010;
      break;
    case GBM_FORMAT_YCbCr_420_TP10_UBWC:
      ret = SDM_BUFFER_FORMAT_YCbCr_420_TP10_UBWC;
      break;
    case GBM_FORMAT_YCbCr_420_P010_UBWC:
      ret = SDM_BUFFER_FORMAT_YCbCr_420_P010_UBWC;
      break;
    case GBM_FORMAT_YCbCr_420_P010_VENUS:
      ret = SDM_BUFFER_FORMAT_YCbCr_420_P010_VENUS;
      break;
    case GBM_FORMAT_P010:
      ret = SDM_BUFFER_FORMAT_P010;
      break;
    default:
      DLOGE("Unsupported GBM format %s\n", FourccToString(fmt));
      break;
  }

  return ret;
}

bool SdmDisplay::NeedConvertGbmFormat(struct weston_view *ev, uint32_t format) {
  pixman_region32_t r;
  bool need_convert = false;

  if (IsTransparentGbmFormat(format)) {
    pixman_region32_init_rect(&r, 0, 0,
                              ev->surface->width,
                              ev->surface->height);
    pixman_region32_subtract(&r, &r, &ev->surface->opaque);

    if (!pixman_region32_not_empty(&r))
      need_convert = true;

    pixman_region32_fini(&r);
  }

  return need_convert;
}

uint32_t SdmDisplay::ConvertToOpaqueGbmFormat(uint32_t format) {
  uint32_t ret = GBM_FORMAT_XRGB8888;

  switch (format) {
    case GBM_FORMAT_ARGB8888:
      ret = GBM_FORMAT_XRGB8888;
      break;
    case GBM_FORMAT_BGRA8888:
      ret = GBM_FORMAT_BGRX8888;
      break;
    case GBM_FORMAT_RGBA8888:
      ret = GBM_FORMAT_RGBX8888;
      break;
    case GBM_FORMAT_ABGR8888:
      ret = GBM_FORMAT_XBGR8888;
      break;
    default:
      break;
  }

  return ret;
}

void SdmDisplay::ComputeSrcDstRect(struct drm_output *output, struct weston_paint_node *node,
                                   struct Rect *src_ret, struct Rect *dst_ret) {
  struct weston_view *ev = node->view;
  struct weston_buffer_viewport *viewport = &ev->surface->buffer_viewport;
  pixman_region32_t dest_rect;
  pixman_box32_t *box;
  struct weston_coord corners[2];
  float sxf1, syf1, sxf2, syf2;

  /* dst rect */
  pixman_region32_init(&dest_rect);
  pixman_region32_intersect(&dest_rect, &ev->transform.boundingbox, &output->base.region);

  weston_matrix_transform_region(&dest_rect, &output->base.matrix, &dest_rect);
  box = pixman_region32_extents(&dest_rect);

  dst_ret->left = (float)box->x1;
  dst_ret->top = (float)box->y1;
  dst_ret->right = (float)box->x2;
  dst_ret->bottom = (float)box->y2;
  pixman_region32_fini(&dest_rect);

  /* src rect */
  corners[0] = weston_matrix_transform_coord(
      &node->output_to_buffer_matrix,
      weston_coord(box->x1, box->y1));
  corners[1] = weston_matrix_transform_coord(
      &node->output_to_buffer_matrix,
      weston_coord(box->x2, box->y2));
  sxf1 = corners[0].x;
  syf1 = corners[0].y;
  sxf2 = corners[1].x;
  syf2 = corners[1].y;

  /* Buffer transforms may mean that x2 is to the left of x1, and/or that
   * y2 is above y1. */
  if (sxf2 < sxf1) {
    double tmp = sxf1;
    sxf1 = sxf2;
    sxf2 = tmp;
  }
  if (syf2 < syf1) {
    double tmp = syf1;
    syf1 = syf2;
    syf2 = tmp;
  }

  src_ret->left = (float)(sxf1);
  src_ret->top = (float)(syf1);
  src_ret->right = (float)(sxf2);
  src_ret->bottom = (float)(syf2);
}

int SdmDisplay::ComputeDirtyRegion(struct weston_view *ev,
                                   struct RectArray *dirty) {
  pixman_region32_t temp;
  struct weston_surface *es = ev->surface;
  pixman_box32_t *rectangles = NULL;
  int n, i;

  pixman_region32_init(&temp);
  pixman_region32_intersect_rect(&temp, &es->damage, 0,
                                 0, es->width, es->height);
  rectangles = pixman_region32_rectangles(&temp, &n);
  if (!n) {
    pixman_region32_fini(&temp);
    return 0;
  }

  dirty->rects = reinterpret_cast<struct Rect *> \
          (zalloc(n * sizeof(struct Rect)));
  if (dirty->rects == NULL) {
    DLOGE("out of memory for allocating dirty region\n");
    pixman_region32_fini(&temp);
    return -1;
  }
  for (i = 0; i < n; i++) {
    dirty->rects[i].left = (float)rectangles[i].x1;
    dirty->rects[i].top = (float)rectangles[i].y1;
    dirty->rects[i].right = (float)rectangles[i].x2;
    dirty->rects[i].bottom = (float)rectangles[i].y2;
  }
  dirty->count = n;
  pixman_region32_fini(&temp);

  return 0;
}

uint8_t SdmDisplay::GetGlobalAlpha(struct weston_view *ev) {
  if (ev->alpha > 1.0f)
    return 0xFF;
  else if (ev->alpha < 0.0f)
    return 0;
  else
    return (uint8_t)(0xFF * ev->alpha);
}

int SdmDisplay::GetVisibleRegion(struct drm_output *output, struct weston_view *ev,
                                 pixman_region32_t *aboved_opaque,
                                 struct RectArray *visible) {
  pixman_region32_t temp;
  pixman_box32_t *rectangles = NULL;
  int n, i;

  pixman_region32_init(&temp);
  pixman_region32_copy(&temp, &ev->transform.boundingbox);
  pixman_region32_subtract(&temp, &temp, aboved_opaque);
  pixman_region32_intersect(&temp, &output->base.region, &temp);
  pixman_region32_translate(&temp, -output->base.pos.c.x, -output->base.pos.c.y);

  rectangles = pixman_region32_rectangles(&temp, &n);
  if (!n) {
    pixman_region32_fini(&temp);
    return 0;
  }

  visible->rects = reinterpret_cast<struct Rect *> (zalloc(n * sizeof(struct Rect)));
  if (visible->rects == NULL) {
    DLOGE("out of memory for allocating visible region\n");
    pixman_region32_fini(&temp);
    return -1;
  }
  for (i = 0; i < n; i++) {
    visible->rects[i].left = (float)rectangles[i].x1;
    visible->rects[i].top = (float)rectangles[i].y1;
    visible->rects[i].right = (float)rectangles[i].x2;
    visible->rects[i].bottom = (float)rectangles[i].y2;
  }
  visible->count = n;
  pixman_region32_fini(&temp);

  return 0;
}

bool SdmDisplay::IsTransparentGbmFormat(uint32_t format) {
  return false;
}

const char *SdmDisplay::GetDisplayString() {
  switch (display_type_) {
    case kPrimary:
      return "primary";
    case kHDMI:
        return "hdmi";
    case kVirtual:
      return "virtual";
    default:
      return "invalid";
  }
}

DisplayError SdmDisplay::EnablePllUpdate(int32_t enable) {
  return kErrorNone;
}

DisplayError SdmDisplay::UpdateDisplayPll(int32_t ppm) {
  return kErrorNone;
}

DisplayError SdmDisplay::GetHdrInfo(struct DisplayHdrInfo *display_hdr_info) {
  DisplayError error;

  DisplayConfigFixedInfo fixed_info = {};
  error = display_intf_->GetConfig(&fixed_info);

  if (error != kErrorNone) {
    DLOGE("Failed to get fixed info. Error = %d", error);
    return error;
  }

  hdr_supported_ = fixed_info.hdr_supported;

  if (!fixed_info.hdr_supported) {
    DLOGI("HDR is not supported");
    return error;
  }

  static const float kLuminanceFactor = 10000.0;
  // luminance is expressed in the unit of 0.0001 cd/m2, convert it to 1cd/m2.
  max_luminance_ = FLOAT(fixed_info.max_luminance)/kLuminanceFactor;
  max_average_luminance_ = FLOAT(fixed_info.average_luminance)/kLuminanceFactor;
  min_luminance_ = FLOAT(fixed_info.min_luminance)/kLuminanceFactor;

  display_hdr_info->hdr_supported = fixed_info.hdr_supported;
  display_hdr_info->hdr_eotf = fixed_info.hdr_eotf;
  display_hdr_info->hdr_metadata_type_one = fixed_info.hdr_metadata_type_one;
  display_hdr_info->max_luminance = fixed_info.max_luminance;
  display_hdr_info->average_luminance = fixed_info.average_luminance;
  display_hdr_info->min_luminance = fixed_info.min_luminance;

  return error;
}

SdmNullDisplay::SdmNullDisplay(int32_t display_id, DisplayType type, CoreInterface *core_intf) {
}

SdmNullDisplay::~SdmNullDisplay() {
}

DisplayError SdmNullDisplay::CreateDisplay() {
  return kErrorNone;
}
DisplayError SdmNullDisplay::DestroyDisplay() {
  return kErrorNone;
}
DisplayError SdmNullDisplay::Prepare(struct drm_output *output) {
  return kErrorNone;
}
DisplayError SdmNullDisplay::Commit(struct drm_output *output) {
  return kErrorNone;
}
DisplayError SdmNullDisplay::Flush() {
  return kErrorNone;
}
DisplayError SdmNullDisplay::SetDisplayState(DisplayState state) {
  return kErrorNone;
}

DisplayError SdmNullDisplay::SetVSyncState(bool enable, struct drm_output *output) {
  return kErrorNone;
}

DisplayError SdmNullDisplay::SetHead(struct drm_head *head) {
  return kErrorNone;
}
DisplayError SdmNullDisplay::GetDisplayConfiguration(struct DisplayConfigInfo *display_config) {
  return kErrorNone;
}
DisplayError SdmNullDisplay::RegisterCb(int display_id, pageflip_cb_t pflipcb) {
  return kErrorNone;
}
DisplayError SdmNullDisplay::EnablePllUpdate(int32_t enable) {
  return kErrorNone;
}
DisplayError SdmNullDisplay::UpdateDisplayPll(int32_t ppm) {
  return kErrorNone;
}
DisplayError SdmNullDisplay::GetHdrInfo(struct DisplayHdrInfo *display_hdr_info) {
  return kErrorNone;
}

SdmDisplayProxy::SdmDisplayProxy(int32_t display_id, DisplayType type, CoreInterface *core_intf)
  : display_id_(display_id), disp_type_(type), core_intf_(core_intf),
  sdm_disp_(display_id, type, core_intf), null_disp_(display_id, type, core_intf) {
  display_intf_ = &sdm_disp_;

  /* todo uevent to handle in another thread*/
  //std::thread uevent_thread(UeventThread, this);
  //uevent_thread_.swap(uevent_thread);
}

SdmDisplayProxy::~SdmDisplayProxy () {
  /* todo uevent to handle in another thread*/
  //uevent_thread_exit_ = true;
  //uevent_thread_.detach();
}

DisplayError  SdmDisplayProxy::HandleHotplug(bool connected) {
  DisplayError error = kErrorNone;
  struct drm_head *head = NULL;

  DLOGI("HandleHotplug = %d", connected);

  if (connected) {
    if (display_intf_->GetDisplayIntfType() == null_disp) {
      display_intf_ = &sdm_disp_;
      error = display_intf_->CreateDisplay();
      if (error != kErrorNone) {
        DLOGE("Failed to create display %d", error);
        display_intf_ = &null_disp_;
        return error;
      }
      head = display_intf_->GetHead();
      display_intf_->SetDisplayState(kStateOn);

      if (hotplug_cb_) {
        hotplug_cb_(display_id_, connected, head);
      }

      DLOGI("Display is connected successfully.");
    } else {
      DLOGI("Display is already connected.");
    }
  } else {
    if (display_intf_->GetDisplayIntfType() == sdm_disp) {
      head = display_intf_->GetHead();
      if (hotplug_cb_) {
        hotplug_cb_(display_id_, connected, head);
      }

      display_intf_->DestroyDisplay();
      display_intf_ = &null_disp_;

      DLOGI("Display is disconnected successfully.");
    } else {
      DLOGI("Display is already disconnected.");
    }
  }

  return error;
}

void *SdmDisplayProxy::UeventThread(void *context) {
  if (context) {
    return reinterpret_cast<SdmDisplayProxy *>(context)->UeventThreadHandler();
  }

  return NULL;
}

#define HAL_PRIORITY_URGENT_DISPLAY (-8)

void *SdmDisplayProxy::UeventThreadHandler() {
  static char uevent_data[4096];
  int length = 0;
  prctl(PR_SET_NAME, uevent_thread_name_, 0, 0, 0);
  setpriority(PRIO_PROCESS, 0, HAL_PRIORITY_URGENT_DISPLAY);
  if (!uevent_init()) {
    DLOGE("Failed to init uevent");
  pthread_exit(0);
  return NULL;
  }

  DLOGI("SdmDisplayProxy::UeventThreadHandler");

  while (!uevent_thread_exit_) {
    // keep last 2 zeroes to ensure double 0 termination
    length = uevent_next_event(uevent_data, INT32(sizeof(uevent_data)) - 2);
    string s(uevent_data, length);

    if (s.find("name=HDMI-A-1") != string::npos) {
      bool connected = s.find("status=connected") != string::npos;
      DLOGI("HDMI is %s !\n", connected ? "connected" : "removed");
      HandleHotplug(connected);
    }
  }
  pthread_exit(0);

  return NULL;
}
#ifdef __cplusplus
}
#endif

}  // namespace sdm
