/*
 * gvm-touch-proxy-internal.h
 *
 * Internal types, constants, and function declarations for gvm-touch-proxy.
 *
 * Included by both gvm-touch-proxy.c (production build) and
 * gvm-touch-proxy-test.c (unit tests), allowing tests to link against the
 * compiled implementation object instead of #include-ing the .c file directly.
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef GVM_TOUCH_PROXY_INTERNAL_H
#define GVM_TOUCH_PROXY_INTERNAL_H

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <wayland-client.h>

/* --- Configuration constants --- */
#define MAX_TOUCH_SLOTS  10    /* MT Protocol B slots per uinput device (max simultaneous fingers) */
#define MAX_PORTS        32    /* max GVM ports (uinput devices) managed by this proxy */
#define MAX_OUTPUTS      16    /* max Weston outputs (displays) tracked for coordinate mapping */
#define MAX_LINE         256   /* line buffer size for config file parsing */
#define MAX_PORT_WIDTH   8192  /* max port width parsed from config; guards against int overflow in buffer size calc */
#define MAX_PORT_HEIGHT  8192  /* max port height parsed from config; guards against int overflow in buffer size calc */
#define GVM_TOUCH_LOG_PREFIX "[GVM-TOUCH] "
#define GVM_TOUCH_LOG_FILE "/tmp/gvm-touch-proxy.log"

/*
 * Stable uinput identity for GVM touch devices so udev rules can match
 * reliably and assign ID_SEAT outside this process.
 */
#define GVM_TOUCH_UINPUT_VENDOR_ID  0x1d6b
#define GVM_TOUCH_UINPUT_PRODUCT_ID 0x7001

#define GVM_TOUCH_PROXY_ENABLE_TOUCH_ONLY_SURFACE 1

#define GVM_TOUCH_WAYLAND_RETRY_INTERVAL_S 1  /* seconds to wait before retrying a lost/failed Wayland session */

#define MIN_U32(a, b) ((a) < (b) ? (a) : (b))

/* fallback for older sysroots that lack <linux/memfd.h> */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

#define PORT_TYPE_HOST   0
#define PORT_TYPE_SHARED 1

/* --- Enums --- */
enum gvm_touch_log_control_mode {
	GVM_TOUCH_LOG_CONTROL_STDOUT = 0,
	GVM_TOUCH_LOG_CONTROL_FILE   = 1,
	GVM_TOUCH_LOG_CONTROL_BOTH   = 2,
};

/* --- Forward declarations of protocol-generated types (pointer-only use) --- */
struct ivi_application;
struct ivi_surface;
struct ivi_input;
struct ivi_wm;
struct ivi_wm_screen;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct touch_only_surface;

/* --- Forward declarations of internal types --- */
typedef struct port_info PortInfo;
struct client_state;
struct touch_route;
struct gtp_seat;

/* --- Struct definitions --- */
struct port_info {
	uint32_t client_id;
	char output_name[64];
	uint32_t ivi_surface_id;
	uint32_t ivi_layer_id;
	char cport_name[64];
	uint32_t display_node;
	uint32_t device_id;
	uint32_t display_id;
	int zorder_base;
	int zorder_size;
	int port_type;

	int offset_x;
	int offset_y;
	int width;
	int height;

	struct wl_surface  *surface;
	struct ivi_surface *ivi_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	bool xdg_configured;
	struct wl_buffer   *buffer;
	int   buffer_fd;
	void *buffer_data;
	int   buffer_stride;
	int   buffer_size;

	struct ivi_wm_screen *ivi_wm_screen;
	struct client_state  *state_ref;
	uint32_t *screen_layer_ids;
	size_t    screen_layer_count;
	size_t    screen_layer_capacity;
	bool      screen_layer_refresh_in_progress;
	uint32_t *ordered_layer_ids;
	size_t    ordered_layer_capacity;

	int uinput_fd;
	int active_touches;

	bool input_acceptance_applied;
	int  last_ivi_input_acceptance;
	bool input_focus_applied;
	int  last_ivi_input_focus;
	char last_applied_seat_name[64];

	PortInfo *host_port;
};

typedef struct {
	struct wl_output *output;
	uint32_t registry_name;
	uint32_t bound_version;
	char     name[64];
	int      width;
	int      height;
} OutputInfo;

struct touch_route {
	bool      active;
	int32_t   id;
	PortInfo *port;
	struct gtp_seat *seat;
};

struct gtp_seat {
	struct client_state *state;
	struct wl_seat  *seat;
	struct wl_touch *touch;
	uint32_t registry_name;
	char     name[64];
	struct gtp_seat *next;
};

struct client_state {
	struct wl_display    *display;
	struct wl_registry   *registry;
	struct wl_compositor *compositor;
	struct wl_shm        *shm;
	struct gtp_seat      *seats;

	struct ivi_application  *ivi_app;
	struct xdg_wm_base      *xdg_wm_base;
	struct ivi_input        *ivi_input;
	struct ivi_wm           *ivi_wm;
	struct touch_only_surface *touch_only;

	uint32_t  host_client_id;
	PortInfo  ports[MAX_PORTS];
	int       port_count;

	OutputInfo outputs[MAX_OUTPUTS];
	int        output_count;

	bool use_ivi_shell;
	bool ivi_window_management;
	bool ivi_zorder_auto;
	int  ivi_input_acceptance;
	int  ivi_input_focus;
	bool touch_log_enabled;
	int  log_control_mode;
	FILE *log_fp;
	char  active_seat_name[64];
	struct touch_route touch_routes[MAX_TOUCH_SLOTS];

	int dummy_pool_fd;
};

/* --- Log macros --- */
#define GTP_LOG_INFO(state, fmt, ...) \
	gvm_touch_log((state), stdout, fmt, ##__VA_ARGS__)

#define GTP_LOG_ERROR(state, fmt, ...) \
	gvm_touch_log((state), stderr, fmt, ##__VA_ARGS__)

#define GTP_PERROR(state, prefix) \
	gvm_touch_log_errno((state), (prefix))

/* --- Internal function declarations --- */
void   gvm_touch_log(struct client_state *state, FILE *fallback_stream,
			       const char *fmt, ...);
void   gvm_touch_log_errno(struct client_state *state,
				      const char *prefix);
bool   gtp_init_log_output(struct client_state *state);
void   gtp_close_log_output(struct client_state *state);
void   gtp_log_touch_event(struct client_state *state, PortInfo *port,
				      const char *event, int32_t id, int x, int y);
void   gtp_log_touch_raw_coordinates(struct client_state *state,
						 PortInfo *port,
						 const char *event, int32_t id,
						 wl_fixed_t x_w, wl_fixed_t y_w);
void   gtp_resolve_port_geometry(struct client_state *state,
					    PortInfo *port);
bool   gtp_parse_ini_file(const char *filename,
				     struct client_state *state);
void   gtp_link_ports(struct client_state *state);
int    gtp_compare_shared_ports_by_zorder(const void *a,
						      const void *b);
bool   gtp_validate_shared_port_configuration(
					struct client_state *state);
void   emit_touch_event(int fd, int type, int code, int value);
struct touch_route *find_touch_route_by_id(struct client_state *state,
						       struct gtp_seat *seat,
						       int32_t id);
PortInfo *find_port_by_surface(struct client_state *state,
					  struct wl_surface *surface);
void   calculate_zorder(struct client_state *state);
bool   is_shared_layer(struct client_state *state, uint32_t layer_id);
void   track_screen_layer(PortInfo *host_port, uint32_t layer_id);
void   untrack_screen_layer(PortInfo *host_port, uint32_t layer_id);
void   apply_ivi_input_policy_for_port(struct client_state *state,
						   PortInfo *port);

/* ivi-input listener callbacks */
void ivi_input_handle_seat_created(void *data,
	struct ivi_input *ivi_input,
	const char *name, uint32_t capabilities, int32_t is_default);
void ivi_input_handle_seat_capabilities(void *data,
	struct ivi_input *ivi_input,
	const char *name, uint32_t capabilities);
void ivi_input_handle_seat_destroyed(void *data,
	struct ivi_input *ivi_input, const char *name);
void ivi_input_handle_input_focus(void *data,
	struct ivi_input *ivi_input,
	uint32_t surface, uint32_t device, int32_t enabled);
void ivi_input_handle_input_acceptance(void *data,
	struct ivi_input *ivi_input,
	uint32_t surface, const char *seat, int32_t accepted);

/* ivi-wm listener callbacks */
void ivi_wm_handle_surface_created(void *data,
	struct ivi_wm *ivi_wm, uint32_t surface_id);
void ivi_wm_handle_surface_destroyed(void *data,
	struct ivi_wm *ivi_wm, uint32_t surface_id);
void ivi_wm_handle_surface_visibility(void *data,
	struct ivi_wm *ivi_wm, uint32_t surface_id, int32_t visibility);
void ivi_wm_handle_surface_opacity(void *data,
	struct ivi_wm *ivi_wm, uint32_t surface_id, wl_fixed_t opacity);
void ivi_wm_handle_surface_source_rectangle(void *data,
	struct ivi_wm *ivi_wm, uint32_t surface_id,
	int32_t x, int32_t y, int32_t width, int32_t height);
void ivi_wm_handle_surface_destination_rectangle(void *data,
	struct ivi_wm *ivi_wm, uint32_t surface_id,
	int32_t x, int32_t y, int32_t width, int32_t height);
void ivi_wm_handle_surface_size(void *data,
	struct ivi_wm *ivi_wm, uint32_t surface_id,
	int32_t width, int32_t height);
void ivi_wm_handle_surface_stats(void *data,
	struct ivi_wm *ivi_wm, uint32_t surface_id,
	uint32_t frame_count, uint32_t pid);
void ivi_wm_handle_surface_error(void *data,
	struct ivi_wm *ivi_wm, uint32_t object_id,
	uint32_t error, const char *message);
void ivi_wm_handle_layer_created(void *data,
	struct ivi_wm *ivi_wm, uint32_t layer_id);
void ivi_wm_handle_layer_destroyed(void *data,
	struct ivi_wm *ivi_wm, uint32_t layer_id);
void ivi_wm_handle_layer_visibility(void *data,
	struct ivi_wm *ivi_wm, uint32_t layer_id, int32_t visibility);
void ivi_wm_handle_layer_opacity(void *data,
	struct ivi_wm *ivi_wm, uint32_t layer_id, wl_fixed_t opacity);
void ivi_wm_handle_layer_source_rectangle(void *data,
	struct ivi_wm *ivi_wm, uint32_t layer_id,
	int32_t x, int32_t y, int32_t width, int32_t height);
void ivi_wm_handle_layer_destination_rectangle(void *data,
	struct ivi_wm *ivi_wm, uint32_t layer_id,
	int32_t x, int32_t y, int32_t width, int32_t height);
void ivi_wm_handle_layer_surface_added(void *data,
	struct ivi_wm *ivi_wm, uint32_t layer_id, uint32_t surface_id);
void ivi_wm_handle_layer_error(void *data,
	struct ivi_wm *ivi_wm, uint32_t object_id,
	uint32_t error, const char *message);
void ivi_wm_screen_handle_screen_id(void *data,
	struct ivi_wm_screen *screen, uint32_t id);
void ivi_wm_screen_handle_connector_name(void *data,
	struct ivi_wm_screen *screen, const char *connector_name);
void ivi_wm_screen_handle_error(void *data,
	struct ivi_wm_screen *screen, uint32_t error, const char *message);
void ivi_wm_screen_handle_layer_added(void *data,
	struct ivi_wm_screen *screen, uint32_t layer_id);

#endif /* GVM_TOUCH_PROXY_INTERNAL_H */
