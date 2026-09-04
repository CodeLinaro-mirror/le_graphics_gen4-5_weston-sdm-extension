/*
 * GVM Touch Proxy - Multi-VM Multi-Display Touch Event Handler
 * Supports IVI-Shell & XDG-Shell
 *
 * Z-Order Management:
 *   The correct z-order relationship between VM windows (GVM) and the host
 *   window (PVM) must be managed by the user/integrator. This program only
 *   provides built-in z-order management for IVI-Shell, intended for testing
 *   purposes. For production use, or when running under XDG-Shell, the user
 *   is responsible for setting up the appropriate z-order.
 *   IVI window management can be disabled via the [host_client] section in
 *   shared_touch.ini (set ivi_window_management = 0).
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <signal.h>
#include <poll.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <wayland-client.h>
#include <libxml/parser.h>
#include <libxml/tree.h>

/* Protocols */
#include "ivi-application-client-protocol.h"
#include "ivi-input-client-protocol.h"
#include "ivi-wm-client-protocol.h"
#include "touch-only-surface-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "gvm-touch-proxy-internal.h"

/* Forward declarations used by Wayland session recovery helpers */
static struct wl_display *connect_to_wayland_display_with_retry(void);
void emit_touch_event(int fd, int type, int code, int value);
static void clear_touch_route(struct touch_route *route);
static void create_surfaces(struct client_state *state);
static void setup_ivi_window_management(struct client_state *state);
static const struct wl_registry_listener registry_listener;

static void
xdg_wm_base_handle_ping(void *data, struct xdg_wm_base *xdg_wm_base,
			 uint32_t serial)
{
	(void) data;
	xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_handle_ping,
};

static volatile sig_atomic_t gvm_touch_running = 1;

void
gvm_touch_log(struct client_state *state, FILE *fallback_stream,
	      const char *fmt, ...)
{
	va_list ap_stream;
	va_list ap_file;
	int mode;
	FILE *file_fp = NULL;
	FILE *stream = fallback_stream ? fallback_stream : stdout;

	mode = state ? state->log_control_mode : GVM_TOUCH_LOG_CONTROL_STDOUT;
	if (state)
		file_fp = state->log_fp;

	va_start(ap_stream, fmt);

	if ((mode == GVM_TOUCH_LOG_CONTROL_FILE ||
	     mode == GVM_TOUCH_LOG_CONTROL_BOTH) && file_fp) {
		va_copy(ap_file, ap_stream);
		vfprintf(file_fp, fmt, ap_file);
		fflush(file_fp);
		va_end(ap_file);
	}

	if (mode == GVM_TOUCH_LOG_CONTROL_STDOUT ||
	    mode == GVM_TOUCH_LOG_CONTROL_BOTH ||
	    ((mode == GVM_TOUCH_LOG_CONTROL_FILE) && !file_fp)) {
		vfprintf(stream, fmt, ap_stream);
		fflush(stream);
	}

	va_end(ap_stream);
}

void
gvm_touch_log_errno(struct client_state *state, const char *prefix)
{
	gvm_touch_log(state, stderr, "%s: %s\n", prefix, strerror(errno));
}


bool
gtp_init_log_output(struct client_state *state)
{
	if (!state)
		return false;

	if (state->log_control_mode == GVM_TOUCH_LOG_CONTROL_FILE ||
	    state->log_control_mode == GVM_TOUCH_LOG_CONTROL_BOTH) {
		state->log_fp = fopen(GVM_TOUCH_LOG_FILE, "a");
		if (!state->log_fp) {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX
				      "Warning: failed to open log file %s: %s, fallback to direct output only\n",
				      GVM_TOUCH_LOG_FILE, strerror(errno));
			state->log_control_mode = GVM_TOUCH_LOG_CONTROL_STDOUT;
			return false;
		}

		setvbuf(state->log_fp, NULL, _IOLBF, 0);
	}

	return true;
}

void
gtp_close_log_output(struct client_state *state)
{
	if (!state || !state->log_fp)
		return;

	fclose(state->log_fp);
	state->log_fp = NULL;
}

void
gtp_log_touch_event(struct client_state *state, PortInfo *port, const char *event,
		int32_t id, int x, int y)
{
	if (!state || !state->touch_log_enabled || !port)
		return;

	gvm_touch_log(state, stdout,
		      GVM_TOUCH_LOG_PREFIX
		      ">>> TOUCH %s: portname=%s uinput_fd=%d id=%d x=%d y=%d active_touches=%d\n",
		      event,
		      port->cport_name[0] ? port->cport_name : port->output_name,
		      port->uinput_fd, id, x, y, port->active_touches);
}

void
gtp_log_touch_raw_coordinates(struct client_state *state, PortInfo *port,
			      const char *event, int32_t id,
			      wl_fixed_t x_w, wl_fixed_t y_w)
{
	if (!state || !state->touch_log_enabled || !port)
		return;

	gvm_touch_log(state, stdout,
		      GVM_TOUCH_LOG_PREFIX
		      ">>> TOUCH %s RAW: portname=%s id=%d raw_x=%f raw_y=%f int_x=%d int_y=%d\n",
		      event,
		      port->cport_name[0] ? port->cport_name : port->output_name,
		      id,
		      wl_fixed_to_double(x_w), wl_fixed_to_double(y_w),
		      wl_fixed_to_int(x_w), wl_fixed_to_int(y_w));
}

/* --- Helpers --- */
OutputInfo *
find_output_by_name(struct client_state *state, const char *name)
{
	int i;

	for (i = 0; i < state->output_count; i++) {
		if (state->outputs[i].name[0] == '\0')
			continue;

		if (strcmp(state->outputs[i].name, name) == 0)
			return &state->outputs[i];
	}

	return NULL;
}

static OutputInfo *
find_output_by_index(struct client_state *state, uint32_t index)
{
	if (!state || index >= (uint32_t) state->output_count)
		return NULL;

	return &state->outputs[index];
}

OutputInfo *
find_output_for_port(struct client_state *state, PortInfo *port)
{
	OutputInfo *out = NULL;

	if (!state || !port)
		return NULL;

	if (port->cport_name[0] != '\0')
		out = find_output_by_name(state, port->cport_name);

	if (!out && port->display_node > 0)
		out = find_output_by_index(state, port->display_node - 1);

	if (!out && port->display_id > 0)
		out = find_output_by_index(state, port->display_id - 1);

	if (!out && state->output_count == 1)
		out = &state->outputs[0];

	return out;
}

PortInfo *
find_host_port_for_output(struct client_state *state, const char *output_name)
{
	int i;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		if (port->port_type != PORT_TYPE_HOST)
			continue;

		if (strcmp(port->cport_name, output_name) == 0)
			return port;
	}

	return NULL;
}

void
gtp_resolve_port_geometry(struct client_state *state, PortInfo *port)
{
	OutputInfo *out;

	if (port->width > 0 && port->height > 0)
		return;

	out = find_output_by_name(state, port->cport_name);
	if (out) {
		if (port->width == 0)
			port->width = out->width;
		if (port->height == 0)
			port->height = out->height;
	}

	if (port->port_type == PORT_TYPE_SHARED && port->host_port) {
		if (port->width == 0)
			port->width = port->host_port->width;
		if (port->height == 0)
			port->height = port->host_port->height;
		if (port->offset_x == 0)
			port->offset_x = port->host_port->offset_x;
		if (port->offset_y == 0)
			port->offset_y = port->host_port->offset_y;
	}

	if (port->width == 0)
		port->width = 1920;
	if (port->height == 0)
		port->height = 1080;
}

const char *
resolve_ini_config_path(char *path, size_t path_size)
{
	const char *usrbin_path = "/usr/bin/shared_touch.ini";
	const char *local_path = "shared_touch.ini";

	if (access(usrbin_path, R_OK) == 0) {
		snprintf(path, path_size, "%s", usrbin_path);
		return path;
	}

	GTP_LOG_ERROR(NULL,
		      GVM_TOUCH_LOG_PREFIX
		      "Warning: %s not found or not readable, fallback to %s\n",
		      usrbin_path, local_path);

	if (access(local_path, R_OK) == 0) {
		snprintf(path, path_size, "%s", local_path);
		return path;
	}

	GTP_LOG_ERROR(NULL,
		      GVM_TOUCH_LOG_PREFIX
		      "Error: %s is also not found or not readable\n",
		      local_path);
	return NULL;
}

const char *
resolve_xml_config_path(char *path, size_t path_size)
{
	const char *var_path    = "/var/qcdisplaycfg.xml";
	const char *usrbin_path = "/usr/bin/qcdisplaycfg.xml";

	/* /var/ override mirrors openwfd (mdss_drvconfig.c) search order:
	 * /var/ first so on-device config changes affect both processes. */
	if (access(var_path, R_OK) == 0) {
		snprintf(path, path_size, "%s", var_path);
		return path;
	}

	if (access(usrbin_path, R_OK) == 0) {
		snprintf(path, path_size, "%s", usrbin_path);
		return path;
	}

	GTP_LOG_ERROR(NULL,
		      GVM_TOUCH_LOG_PREFIX
		      "Error: %s and %s not found or not readable\n",
		      var_path, usrbin_path);
	return NULL;
}

/* --- INI Parser --- */
bool
gtp_parse_ini_file(const char *filename, struct client_state *state)
{
	FILE *fp = fopen(filename, "r");
	char line[MAX_LINE];
	bool in_host = false;
	bool in_vm = false;
	uint32_t current_vm_id = 0;
	int line_num = 0;

	if (!fp) {
		GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX "Error: Cannot open %s\n", filename);
		return false;
	}

	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> Parsing INI file: %s\n", filename);

	while (fgets(line, sizeof(line), fp)) {
		char *eq;
		char *key;
		char *value;

		line_num++;
		line[strcspn(line, "\r\n")] = 0;
		GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> INI line %d: '%s'\n", line_num, line);

		if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
			continue;

		if (strcmp(line, "[host_client]") == 0) {
			in_host = true;
			in_vm = false;
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> Enter [host_client] section\n");
			continue;
		} else if (strcmp(line, "[vm_client]") == 0) {
			in_host = false;
			in_vm = true;
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> Enter [vm_client] section\n");
			continue;
		}

		eq = strchr(line, '=');
		if (!eq) {
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> Skip line %d without '='\n", line_num);
			continue;
		}

		*eq = '\0';
		key = line;
		value = eq + 1;

		while (*key == ' ' || *key == '\t')
			key++;
		while (*value == ' ' || *value == '\t')
			value++;

		{
			char *orig_key = key;
			char *orig_value = value;
			char *key_end = key + strlen(key);
			char *value_end = value + strlen(value);

			while (key_end > key &&
			       (key_end[-1] == ' ' || key_end[-1] == '\t')) {
				key_end--;
				*key_end = '\0';
			}

			while (value_end > value &&
			       (value_end[-1] == ' ' || value_end[-1] == '\t')) {
				value_end--;
				*value_end = '\0';
			}

			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> Raw key='%s' value='%s' -> Trimmed key='%s' value='%s'\n",
				     orig_key, orig_value, key, value);
		}

		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> Parsed key='%s' value='%s' (host=%d vm=%d)\n",
			     key, value, in_host, in_vm);

		if (in_host && strcmp(key, "id") == 0) {
			sscanf(value, "%x", &state->host_client_id);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> Host Client ID: 0x%X\n",
				     state->host_client_id);
		} else if (in_host && strcmp(key, "ivi_window_management") == 0) {
			int enable = 0;
			sscanf(value, "%d", &enable);
			state->ivi_window_management = (enable == 1);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI Window Management: %s\n",
				     state->ivi_window_management ? "enabled" : "disabled");
		} else if (in_host && strcmp(key, "ivi_zorder_auto") == 0) {
			int enable = 0;
			sscanf(value, "%d", &enable);
			state->ivi_zorder_auto = (enable == 1);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI Z-order Auto: %s\n",
				     state->ivi_zorder_auto ? "enabled" : "disabled");
		} else if (in_host && strcmp(key, "ivi_input_acceptance") == 0) {
			sscanf(value, "%d", &state->ivi_input_acceptance);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI Input Acceptance: %d\n",
				     state->ivi_input_acceptance);
		} else if (in_host && strcmp(key, "ivi_input_focus") == 0) {
			sscanf(value, "%d", &state->ivi_input_focus);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI Input Focus: %d\n",
				     state->ivi_input_focus);
		} else if (in_host && strcmp(key, "touch_log_enabled") == 0) {
			int enable = 0;

			sscanf(value, "%d", &enable);
			state->touch_log_enabled = (enable == 1);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> Touch Log: %s\n",
				     state->touch_log_enabled ? "enabled" : "disabled");
		} else if (in_host && strcmp(key, "log_control_mode") == 0) {
			int mode = GVM_TOUCH_LOG_CONTROL_STDOUT;

			sscanf(value, "%d", &mode);
			if (mode < GVM_TOUCH_LOG_CONTROL_STDOUT ||
			    mode > GVM_TOUCH_LOG_CONTROL_BOTH) {
				GTP_LOG_ERROR(state,
					      GVM_TOUCH_LOG_PREFIX
					      "Warning: invalid log_control_mode=%d, fallback to 0\n",
					      mode);
				mode = GVM_TOUCH_LOG_CONTROL_STDOUT;
			}

			state->log_control_mode = mode;
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> Log Control Mode: %d\n",
				     state->log_control_mode);
		} else if (in_vm) {
			if (strcmp(key, "id") == 0) {
				sscanf(value, "%x", &current_vm_id);
				GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> VM Client ID: 0x%X\n",
					     current_vm_id);
			} else if (strcmp(key, "output") == 0 && current_vm_id != 0) {
				if (state->port_count < MAX_PORTS) {
					PortInfo *port = &state->ports[state->port_count];
					memset(port, 0, sizeof(*port));
					port->client_id = current_vm_id;
					snprintf(port->output_name,
						 sizeof(port->output_name), "%s", value);
					port->port_type = PORT_TYPE_SHARED;
					port->uinput_fd = -1;
					state->port_count++;
				}
			} else if (strcmp(key, "ivi_surface_id") == 0 &&
				   state->port_count > 0) {
				sscanf(value, "%u",
				       &state->ports[state->port_count - 1].ivi_surface_id);
			} else if (strcmp(key, "ivi_layer_id") == 0 &&
				   state->port_count > 0) {
				sscanf(value, "%u",
				       &state->ports[state->port_count - 1].ivi_layer_id);
			}
		}
	}

	fclose(fp);
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> INI parse finished: host_id=0x%X port_count=%d current_vm_id=0x%X\n",
		     state->host_client_id, state->port_count, current_vm_id);
	return state->port_count > 0;
}

/* --- XML Parser --- */
bool
parse_xml_config(const char *filename, struct client_state *state)
{
	xmlDoc *doc = xmlReadFile(filename, NULL, 0);
	xmlNode *root;

	if (!doc) {
		GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX "Error: Cannot parse %s\n", filename);
		return false;
	}

	root = xmlDocGetRootElement(doc);
	if (!root) {
		xmlFreeDoc(doc);
		return false;
	}

	for (xmlNode *node = root->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE &&
		    strcmp((char *) node->name, "WFDConfig") == 0) {
			for (xmlNode *client = node->children; client; client = client->next) {
				xmlChar *id_str;
				uint32_t client_id;
				bool is_host;
				bool is_vm = false;

				if (client->type != XML_ELEMENT_NODE ||
				    strcmp((char *) client->name, "WFDClient") != 0)
					continue;

				id_str = xmlGetProp(client, (xmlChar *) "ID");
				if (!id_str)
					continue;

				sscanf((char *) id_str, "%x", &client_id);
				xmlFree(id_str);

				is_host = (client_id == state->host_client_id);
				for (int i = 0; i < state->port_count; i++) {
					if (state->ports[i].client_id == client_id) {
						is_vm = true;
						break;
					}
				}

				if (!is_host && !is_vm)
					continue;

				for (xmlNode *port_node = client->children; port_node;
				     port_node = port_node->next) {
					if (port_node->type != XML_ELEMENT_NODE ||
					    strcmp((char *) port_node->name, "WFDPort") != 0)
						continue;

					for (xmlNode *attrib = port_node->children; attrib;
					     attrib = attrib->next) {
						xmlChar *cport;
						xmlChar *dnode;
						xmlChar *zbase;
						xmlChar *zsize;
						xmlChar *device;
						xmlChar *display;
						uint32_t display_node;
						uint32_t device_id = 0;
						uint32_t display_id = 0;
						int zorder_base;
						int zorder_size;

						if (attrib->type != XML_ELEMENT_NODE ||
						    strcmp((char *) attrib->name, "PortAttribs") != 0)
							continue;

						cport = xmlGetProp(attrib, (xmlChar *) "cPortName");
						dnode = xmlGetProp(attrib, (xmlChar *) "uDisplayNode");
						zbase = xmlGetProp(attrib, (xmlChar *) "eZOrderBase");
						zsize = xmlGetProp(attrib, (xmlChar *) "uZOrderSize");
						device = xmlGetProp(attrib, (xmlChar *) "eQDIDeviceID");
						display = xmlGetProp(attrib, (xmlChar *) "eQDIDisplayID");

						if (!cport)
							continue;

						display_node = dnode ? atoi((char *) dnode) : 0;
						device_id = device ? (uint32_t) atoi((char *) device) : 0;
						display_id = display ? (uint32_t) atoi((char *) display) : 0;
						zorder_base = zbase ? atoi((char *) zbase) : 0;
						zorder_size = zsize ? atoi((char *) zsize) : 1;

						if (is_host) {
							if (state->port_count < MAX_PORTS) {
								PortInfo *hp = &state->ports[state->port_count];

								memset(hp, 0, sizeof(*hp));
								hp->client_id = client_id;
								snprintf(hp->cport_name,
									 sizeof(hp->cport_name), "%s", (char *) cport);
								hp->display_node = display_node;
								hp->device_id = device_id;
								hp->display_id = display_id;
								hp->zorder_base = zorder_base;
								hp->zorder_size = zorder_size;
								hp->port_type = PORT_TYPE_HOST;
								hp->uinput_fd = -1;
								state->port_count++;
								GTP_LOG_INFO(state,
									     GVM_TOUCH_LOG_PREFIX
									     ">>> Host Port: %s (node=%u, z=%d)\n",
									     hp->cport_name, display_node,
									     zorder_base);
							}
						} else if (is_vm) {
							for (int i = 0; i < state->port_count; i++) {
								PortInfo *sp = &state->ports[i];

								if (sp->client_id == client_id &&
								    sp->port_type == PORT_TYPE_SHARED &&
								    strcmp(sp->output_name,
									   (char *) cport) == 0) {
									snprintf(sp->cport_name,
										 sizeof(sp->cport_name), "%s", (char *) cport);
									sp->display_node = display_node;
									sp->device_id = device_id;
									sp->display_id = display_id;
									sp->zorder_base = zorder_base;
									sp->zorder_size = zorder_size;
									GTP_LOG_INFO(state,
										     GVM_TOUCH_LOG_PREFIX
										     ">>> Shared Port: %s (node=%u, device=%u, display=%u, z=%d, ivi=%u, layer=%u)\n",
										     sp->cport_name, display_node,
										     device_id, display_id,
										     zorder_base, sp->ivi_surface_id,
										     sp->ivi_layer_id);
									break;
								}
							}
						}

						if (cport)
							xmlFree(cport);
						if (dnode)
							xmlFree(dnode);
						if (zbase)
							xmlFree(zbase);
						if (zsize)
							xmlFree(zsize);
						if (device)
							xmlFree(device);
						if (display)
							xmlFree(display);
					}
				}
			}
		}

		if (node->type == XML_ELEMENT_NODE &&
		    strcmp((char *) node->name, "Display") == 0) {
			for (xmlNode *dn = node->children; dn; dn = dn->next) {
				if (dn->type != XML_ELEMENT_NODE ||
				    strcmp((char *) dn->name, "DisplayNode") != 0)
					continue;

				xmlChar *node_id = xmlGetProp(dn, (xmlChar *) "ID");
				if (!node_id)
					continue;

				uint32_t dnode_id = atoi((char *) node_id);
				xmlFree(node_id);

				for (xmlNode *attr = dn->children; attr; attr = attr->next) {
					xmlChar *ox;
					xmlChar *oy;
					xmlChar *vw;
					xmlChar *vh;
					int offset_x;
					int offset_y;
					int width;
					int height;

					if (attr->type != XML_ELEMENT_NODE ||
					    strcmp((char *) attr->name, "Attributes") != 0)
						continue;

					ox = xmlGetProp(attr, (xmlChar *) "uOffsetX");
					oy = xmlGetProp(attr, (xmlChar *) "uOffsetY");
					vw = xmlGetProp(attr, (xmlChar *) "uVisWidth");
					vh = xmlGetProp(attr, (xmlChar *) "uVisHeight");

					offset_x = ox ? atoi((char *) ox) : 0;
					offset_y = oy ? atoi((char *) oy) : 0;
					width = vw ? atoi((char *) vw) : 0;
					height = vh ? atoi((char *) vh) : 0;
					/* clamp to sane max to prevent int overflow in buffer size calc */
					if (width > MAX_PORT_WIDTH)
						width = MAX_PORT_WIDTH;
					if (height > MAX_PORT_HEIGHT)
						height = MAX_PORT_HEIGHT;

					for (int i = 0; i < state->port_count; i++) {
						PortInfo *p = &state->ports[i];

						if (p->display_node == dnode_id) {
							p->offset_x = offset_x;
							p->offset_y = offset_y;
							if (width > 0)
								p->width = width;
							if (height > 0)
								p->height = height;
						}
					}

					if (ox)
						xmlFree(ox);
					if (oy)
						xmlFree(oy);
					if (vw)
						xmlFree(vw);
					if (vh)
						xmlFree(vh);
				}
			}
		}
	}

	xmlFreeDoc(doc);
	return true;
}

/* --- Link shared ports to host ports --- */
void
gtp_link_ports(struct client_state *state)
{
	int i;
	int j;
	int shared_without_host = 0;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *sp = &state->ports[i];
		PortInfo *best_full_match = NULL;
		PortInfo *fallback_match = NULL;
		uint32_t sp_node;

		if (sp->port_type != PORT_TYPE_SHARED)
			continue;

		/* Default display_node to 0 if not set */
		sp_node = sp->display_node ? sp->display_node : 0;
		sp->display_node = sp_node;

		/* Step 1: full match on device/display/displayNode */
		for (j = 0; j < state->port_count; j++) {
			PortInfo *hp = &state->ports[j];

			if (hp->port_type != PORT_TYPE_HOST)
				continue;

			if (hp->client_id != state->host_client_id)
				continue;

			if (hp->device_id == sp->device_id &&
			    hp->display_id == sp->display_id &&
			    hp->display_node == sp_node) {
				best_full_match = hp;
				break;
			}
		}

		/* Step 2: fallback to host port with same device/display and node==0 */
		if (!best_full_match) {
			for (j = 0; j < state->port_count; j++) {
				PortInfo *hp = &state->ports[j];

				if (hp->port_type != PORT_TYPE_HOST)
					continue;

				if (hp->client_id != state->host_client_id)
					continue;

				if (hp->device_id == sp->device_id &&
				    hp->display_id == sp->display_id &&
				    hp->display_node == 0) {
					fallback_match = hp;
					break;
				}
			}
		}

		if (best_full_match) {
			sp->host_port = best_full_match;
			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> Linked shared '%s' (device=%u, display=%u, node=%u) -> host '%s' (device=%u, display=%u, node=%u)\n",
				     sp->cport_name,
				     sp->device_id, sp->display_id, sp->display_node,
				     best_full_match->cport_name,
				     best_full_match->device_id, best_full_match->display_id,
				     best_full_match->display_node);
		} else if (fallback_match) {
			sp->host_port = fallback_match;
			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> Linked shared '%s' (device=%u, display=%u, node=%u) -> host(fallback node=0) '%s' (device=%u, display=%u, node=%u)\n",
				     sp->cport_name,
				     sp->device_id, sp->display_id, sp->display_node,
				     fallback_match->cport_name,
				     fallback_match->device_id, fallback_match->display_id,
				     fallback_match->display_node);
		} else {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX
				      "Error: No host port found for shared '%s' (device=%u, display=%u, node=%u)\n",
				      sp->cport_name,
				      sp->device_id, sp->display_id, sp->display_node);
			shared_without_host++;
		}
	}

	if (shared_without_host > 0) {
		GTP_LOG_ERROR(state,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: %d shared ports have no matching host port (even with fallback). Exiting.\n",
			      shared_without_host);
	}

	for (i = 0; i < state->port_count; i++)
		gtp_resolve_port_geometry(state, &state->ports[i]);
}

int
gtp_compare_shared_ports_by_zorder(const void *a, const void *b)
{
	const PortInfo * const *pa = a;
	const PortInfo * const *pb = b;

	return (*pa)->zorder_base - (*pb)->zorder_base;
}

bool
is_shared_layer(struct client_state *state, uint32_t layer_id)
{
	int i;

	if (layer_id == 0)
		return false;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		if (port->port_type != PORT_TYPE_SHARED)
			continue;

		if (port->ivi_layer_id == layer_id)
			return true;
	}

	return false;
}

static bool
ensure_screen_layer_capacity(PortInfo *host_port, size_t needed)
{
	uint32_t *new_ids;
	size_t new_capacity;

	if (!host_port)
		return false;

	if (needed <= host_port->screen_layer_capacity)
		return true;

	new_capacity = host_port->screen_layer_capacity ?
		       host_port->screen_layer_capacity * 2 : 8;
	while (new_capacity < needed)
		new_capacity *= 2;

	new_ids = realloc(host_port->screen_layer_ids,
			  new_capacity * sizeof(*new_ids));
	if (!new_ids) {
		GTP_LOG_ERROR(host_port->state_ref,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: failed to grow screen layer tracking for host %s to %zu entries\n",
			      host_port->cport_name, new_capacity);
		return false;
	}

	host_port->screen_layer_ids = new_ids;
	host_port->screen_layer_capacity = new_capacity;
	return true;
}

static bool
ensure_ordered_layer_capacity(PortInfo *host_port, size_t needed)
{
	uint32_t *new_ids;
	size_t new_capacity;

	if (!host_port)
		return false;

	if (needed <= host_port->ordered_layer_capacity)
		return true;

	new_capacity = host_port->ordered_layer_capacity ?
		       host_port->ordered_layer_capacity * 2 : 8;
	while (new_capacity < needed)
		new_capacity *= 2;

	new_ids = realloc(host_port->ordered_layer_ids,
			  new_capacity * sizeof(*new_ids));
	if (!new_ids) {
		GTP_LOG_ERROR(host_port->state_ref,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: failed to grow ordered layer buffer for host %s to %zu entries\n",
			      host_port->cport_name, new_capacity);
		return false;
	}

	host_port->ordered_layer_ids = new_ids;
	host_port->ordered_layer_capacity = new_capacity;
	return true;
}

void
track_screen_layer(PortInfo *host_port, uint32_t layer_id)
{
	size_t i;

	if (!host_port || layer_id == 0)
		return;

	for (i = 0; i < host_port->screen_layer_count; i++) {
		if (host_port->screen_layer_ids[i] == layer_id)
			return;
	}

	if (!ensure_screen_layer_capacity(host_port,
					  host_port->screen_layer_count + 1))
		return;

	host_port->screen_layer_ids[host_port->screen_layer_count++] = layer_id;
}

void
untrack_screen_layer(PortInfo *host_port, uint32_t layer_id)
{
	size_t i;

	if (!host_port || layer_id == 0)
		return;

	for (i = 0; i < host_port->screen_layer_count; i++) {
		if (host_port->screen_layer_ids[i] != layer_id)
			continue;

		for (; i + 1 < host_port->screen_layer_count; i++)
			host_port->screen_layer_ids[i] = host_port->screen_layer_ids[i + 1];

		host_port->screen_layer_count--;
		return;
	}
}

static void
refresh_screen_layer_order(PortInfo *host_port)
{
	struct client_state *state;

	if (!host_port || !host_port->ivi_wm_screen)
		return;

	state = host_port->state_ref;
	if (!state || !state->ivi_wm || !state->ivi_zorder_auto)
		return;

	host_port->screen_layer_count = 0;
	host_port->screen_layer_refresh_in_progress = true;
	ivi_wm_screen_get(host_port->ivi_wm_screen, IVI_WM_PARAM_RENDER_ORDER);

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM screen render-order refresh requested: host=%s\n",
		     host_port->cport_name);
}

static void
refresh_all_host_screens(struct client_state *state)
{
	int i;

	if (!state || !state->ivi_wm || !state->ivi_zorder_auto)
		return;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *host_port = &state->ports[i];

		if (host_port->port_type != PORT_TYPE_HOST)
			continue;

		refresh_screen_layer_order(host_port);
	}
}

static void
enforce_zorder_on_screen(struct client_state *state, PortInfo *host_port)
{
	PortInfo *below_shared[MAX_PORTS];
	PortInfo *above_shared[MAX_PORTS];
	size_t ordered_capacity;
	size_t ordered_count = 0;
	int below_count = 0;
	int above_count = 0;
	int i;

	if (!state || !host_port || !host_port->ivi_wm_screen || !state->ivi_wm)
		return;

	ordered_capacity = host_port->screen_layer_count + MAX_PORTS;
	if (!ensure_ordered_layer_capacity(host_port,
					   ordered_capacity ? ordered_capacity : 1))
		return;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		if (port->port_type != PORT_TYPE_SHARED || port->host_port != host_port)
			continue;

		if (port->zorder_base < host_port->zorder_base)
			below_shared[below_count++] = port;
		else
			above_shared[above_count++] = port;
	}

	qsort(below_shared, below_count, sizeof(below_shared[0]),
	      gtp_compare_shared_ports_by_zorder);
	qsort(above_shared, above_count, sizeof(above_shared[0]),
	      gtp_compare_shared_ports_by_zorder);

	ivi_wm_screen_clear(host_port->ivi_wm_screen);

	for (i = 0; i < below_count; i++) {
		if (below_shared[i]->ivi_layer_id == 0)
			continue;
		host_port->ordered_layer_ids[ordered_count++] =
			below_shared[i]->ivi_layer_id;
	}

	for (i = 0; i < (int) host_port->screen_layer_count; i++) {
		uint32_t layer_id = host_port->screen_layer_ids[i];

		if (layer_id == 0 || is_shared_layer(state, layer_id))
			continue;

		host_port->ordered_layer_ids[ordered_count++] = layer_id;
	}

	for (i = 0; i < above_count; i++) {
		if (above_shared[i]->ivi_layer_id == 0)
			continue;
		host_port->ordered_layer_ids[ordered_count++] =
			above_shared[i]->ivi_layer_id;
	}

	for (i = 0; i < (int) ordered_count; i++)
		ivi_wm_screen_add_layer(host_port->ivi_wm_screen,
					host_port->ordered_layer_ids[i]);

	ivi_wm_commit_changes(state->ivi_wm);

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Z-order enforced: host=%s host_layers=%zu below=%d above=%d total=%zu ordered_capacity=%zu\n",
		     host_port->cport_name, host_port->screen_layer_count,
		     below_count, above_count, ordered_count,
		     host_port->ordered_layer_capacity);
}

/* --- Calculate and display Z-order relationships --- */
bool
gtp_validate_shared_port_configuration(struct client_state *state)
{
	int host_count = 0;
	int shared_count = 0;
	int linked_shared_count = 0;
	int i;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		if (port->port_type == PORT_TYPE_HOST)
			host_count++;
		else if (port->port_type == PORT_TYPE_SHARED)
			shared_count++;

		if (port->port_type == PORT_TYPE_SHARED && port->host_port)
			linked_shared_count++;
	}

	if (shared_count == 0) {
		GTP_LOG_ERROR(state,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: No shared displays found in qcdisplaycfg.xml for configured VM clients. Exiting.\n");
		return false;
	}

	if (linked_shared_count == 0) {
		GTP_LOG_ERROR(state,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: shared_touch.ini configured shared ports, but none of them match shared displays in qcdisplaycfg.xml or host ports (device/display/node + fallback). Exiting.\n");
		return false;
	}

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		if (port->port_type != PORT_TYPE_SHARED)
			continue;

		if (port->cport_name[0] == '\0') {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX
				      "Error: shared port '%s' from shared_touch.ini was not found in qcdisplaycfg.xml. Exiting.\n",
				      port->output_name);
			return false;
		}

		if (!port->host_port) {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX
				      "Error: shared port '%s' is present in qcdisplaycfg.xml but is not linked to any host display. Exiting.\n",
				      port->cport_name);
			return false;
		}
	}

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> Shared port validation passed: host_ports=%d shared_ports=%d linked_shared_ports=%d\n",
		     host_count, shared_count, linked_shared_count);
	return true;
}

void
calculate_zorder(struct client_state *state)
{
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "\n=== Z-Order Analysis ===\n");

	for (int i = 0; i < state->port_count; i++) {
		PortInfo *hp = &state->ports[i];
		PortInfo *shared_ports[MAX_PORTS];
		int shared_count = 0;

		if (hp->port_type != PORT_TYPE_HOST)
			continue;

		GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "\nHost Port: %s\n", hp->cport_name);
		GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "  Z-order: base=%d, size=%d\n",
			     hp->zorder_base, hp->zorder_size);
		GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "  Geometry: x=%d, y=%d, w=%d, h=%d\n",
			     hp->offset_x, hp->offset_y, hp->width, hp->height);

		for (int j = 0; j < state->port_count; j++) {
			PortInfo *sp = &state->ports[j];
			if (sp->port_type == PORT_TYPE_SHARED && sp->host_port == hp)
				shared_ports[shared_count++] = sp;
		}

		if (shared_count == 0) {
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "  No shared ports linked\n");
			continue;
		}

		qsort(shared_ports, shared_count, sizeof(shared_ports[0]),
		      gtp_compare_shared_ports_by_zorder);

		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     "  Shared Ports (sorted by z-order base, lower=bottom):\n");
		for (int j = 0; j < shared_count; j++) {
			PortInfo *sp = shared_ports[j];
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "    %s\n", sp->cport_name);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "      Z-order: base=%d, size=%d\n",
				     sp->zorder_base, sp->zorder_size);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "      IVI Surface ID: %u\n",
				     sp->ivi_surface_id);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "      IVI Layer ID: %u\n",
				     sp->ivi_layer_id);
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "      Geometry: x=%d, y=%d, w=%d, h=%d\n",
				     sp->offset_x, sp->offset_y, sp->width, sp->height);
		}
	}

	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "\n=== Z-Order Summary ===\n");
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "Total Ports: %d\n", state->port_count);

	{
		int host_count = 0;
		int shared_count = 0;

		for (int i = 0; i < state->port_count; i++) {
			if (state->ports[i].port_type == PORT_TYPE_HOST)
				host_count++;
			else
				shared_count++;
		}

		GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "Host Ports: %d, Shared Ports: %d\n",
			     host_count, shared_count);
	}

	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX "========================\n\n");
}

/* --- UINPUT SETUP --- */
static int
setup_uinput(PortInfo *port)
{
	struct uinput_setup usetup;
	struct uinput_abs_setup abs_setup;
	int fd;
	int max_x;
	int max_y;
	const char *port_name;

	if (!port)
		return -1;

	if (port->uinput_fd >= 0)
		return port->uinput_fd;

	max_x = port->width > 0 ? port->width : 1920;
	max_y = port->height > 0 ? port->height : 1080;

	port_name = port->cport_name[0] ? port->cport_name : port->output_name;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		GTP_PERROR(NULL, "UINPUT: Failed to open /dev/uinput");
		return -1;
	}

	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

	ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);

	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = GVM_TOUCH_UINPUT_VENDOR_ID;
	usetup.id.product = GVM_TOUCH_UINPUT_PRODUCT_ID;
	usetup.id.version = 0x01;
	snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "GVM Touch %s",
		 port_name);

	if (ioctl(fd, UI_DEV_SETUP, &usetup) == -1) {
		GTP_PERROR(NULL, "UI_DEV_SETUP");
		close(fd);
		return -1;
	}

	memset(&abs_setup, 0, sizeof(abs_setup));
	abs_setup.code = ABS_MT_POSITION_X;
	abs_setup.absinfo.maximum = max_x;
	ioctl(fd, UI_ABS_SETUP, &abs_setup);

	memset(&abs_setup, 0, sizeof(abs_setup));
	abs_setup.code = ABS_MT_POSITION_Y;
	abs_setup.absinfo.maximum = max_y;
	ioctl(fd, UI_ABS_SETUP, &abs_setup);

	memset(&abs_setup, 0, sizeof(abs_setup));
	abs_setup.code = ABS_MT_SLOT;
	abs_setup.absinfo.maximum = MAX_TOUCH_SLOTS - 1;
	ioctl(fd, UI_ABS_SETUP, &abs_setup);

	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		GTP_PERROR(NULL, "UI_DEV_CREATE");
		close(fd);
		return -1;
	}

	port->uinput_fd = fd;

	GTP_LOG_INFO(NULL,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> UINPUT: Created device name='%s' for %s (%dx%d) vid=0x%04x pid=0x%04x\n",
		     usetup.name, port_name, max_x, max_y,
		     GVM_TOUCH_UINPUT_VENDOR_ID, GVM_TOUCH_UINPUT_PRODUCT_ID);
	return fd;
}

bool
setup_shared_port_uinputs(struct client_state *state)
{
	int i;
	bool ok = true;

	if (!state)
		return false;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		if (port->port_type != PORT_TYPE_SHARED)
			continue;

		gtp_resolve_port_geometry(state, port);

		if (setup_uinput(port) < 0) {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX "Error: failed to setup uinput for shared port %s\n",
				      port->cport_name[0] ? port->cport_name : port->output_name);
			ok = false;
		}
	}

	return ok;
}

static struct wl_display *
connect_to_wayland_display_with_retry(void)
{
	struct wl_display *display = NULL;

	while (!display && gvm_touch_running) {
		display = wl_display_connect(NULL);
		if (display)
			break;

		GTP_LOG_ERROR(NULL,
			      GVM_TOUCH_LOG_PREFIX
			      "Waiting for wayland display to become available before creating surfaces...\n");
		sleep(GVM_TOUCH_WAYLAND_RETRY_INTERVAL_S);
	}

	return display;
}

void
emit_touch_event(int fd, int type, int code, int value)
{
	struct input_event ie;
	ssize_t ret;

	memset(&ie, 0, sizeof(ie));
	ie.type = type;
	ie.code = code;
	ie.value = value;
	ret = write(fd, &ie, sizeof(ie));
	if (ret < 0 || (size_t) ret != sizeof(ie))
		GTP_LOG_ERROR(NULL,
			      GVM_TOUCH_LOG_PREFIX
			      "emit_touch_event: write failed: type=%d code=%d value=%d\n",
			      type, code, value);
}

/* --- Find port by surface --- */
PortInfo *
find_port_by_surface(struct client_state *state, struct wl_surface *surface)
{
	int i;

	for (i = 0; i < state->port_count; i++) {
		if (state->ports[i].surface == surface)
			return &state->ports[i];
	}

	return NULL;
}

static struct gtp_seat *
find_seat_by_registry_name(struct client_state *state, uint32_t registry_name)
{
	struct gtp_seat *seat;

	if (!state)
		return NULL;

	for (seat = state->seats; seat; seat = seat->next) {
		if (seat->registry_name == registry_name)
			return seat;
	}

	return NULL;
}

struct gtp_seat *
find_seat_by_name(struct client_state *state, const char *name)
{
	struct gtp_seat *seat;

	if (!state || !name || name[0] == '\0')
		return NULL;

	for (seat = state->seats; seat; seat = seat->next) {
		if (strcmp(seat->name, name) == 0)
			return seat;
	}

	return NULL;
}

static struct gtp_seat *
add_seat(struct client_state *state, struct wl_registry *registry,
	 uint32_t name, uint32_t version)
{
	struct gtp_seat *seat;
	uint32_t bind_version;

	if (!state || !registry)
		return NULL;

	if (find_seat_by_registry_name(state, name))
		return NULL;

	seat = calloc(1, sizeof(*seat));
	if (!seat)
		return NULL;

	bind_version = MIN_U32(version, 5);
	seat->state = state;
	seat->registry_name = name;
	seat->seat = wl_registry_bind(registry, name, &wl_seat_interface,
				      bind_version);
	if (!seat->seat) {
		free(seat);
		return NULL;
	}

	seat->next = state->seats;
	state->seats = seat;

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> Bound wl_seat name=%u bind_version=%u\n",
		     name, bind_version);
	return seat;
}

static void
destroy_all_seats(struct client_state *state)
{
	struct gtp_seat *seat;
	struct gtp_seat *next;

	if (!state)
		return;

	seat = state->seats;
	while (seat) {
		next = seat->next;

		if (seat->touch) {
			wl_touch_destroy(seat->touch);
			seat->touch = NULL;
		}

		if (seat->seat) {
			wl_seat_destroy(seat->seat);
			seat->seat = NULL;
		}

		free(seat);
		seat = next;
	}

	state->seats = NULL;
}

struct touch_route *
find_touch_route_by_id(struct client_state *state, struct gtp_seat *seat, int32_t id)
{
	int i;

	if (!state)
		return NULL;

	for (i = 0; i < MAX_TOUCH_SLOTS; i++) {
		if (!state->touch_routes[i].active)
			continue;

		if (state->touch_routes[i].id == id &&
		    state->touch_routes[i].seat == seat)
			return &state->touch_routes[i];
	}

	return NULL;
}

static struct touch_route *
allocate_touch_route(struct client_state *state, struct gtp_seat *seat, int32_t id, PortInfo *port)
{
	int i;
	struct touch_route *route;

	if (!state || !port)
		return NULL;

	route = find_touch_route_by_id(state, seat, id);
	if (route) {
		route->port = port;
		return route;
	}

	for (i = 0; i < MAX_TOUCH_SLOTS; i++) {
		if (state->touch_routes[i].active)
			continue;

		state->touch_routes[i].active = true;
		state->touch_routes[i].id = id;
		state->touch_routes[i].port = port;
		state->touch_routes[i].seat = seat;
		return &state->touch_routes[i];
	}

	return NULL;
}

static void
clear_touch_route(struct touch_route *route)
{
	if (!route)
		return;

	route->active = false;
	route->id = -1;
	route->port = NULL;
	route->seat = NULL;
}

/* --- IVI Input Helpers --- */
void
apply_ivi_input_policy_for_port(struct client_state *state, PortInfo *port)
{
	bool focus_changed = false;
	struct gtp_seat *seat;
	int accepted_seat_count = 0;

	if (!state->ivi_input || !port || port->ivi_surface_id == 0)
		return;

	for (seat = state->seats; seat; seat = seat->next) {
		if (seat->name[0] == '\0')
			continue;

		ivi_input_set_input_acceptance(state->ivi_input,
					       port->ivi_surface_id,
					       seat->name,
					       state->ivi_input_acceptance);
		accepted_seat_count++;
	}

	port->input_acceptance_applied = (accepted_seat_count > 0);
	port->last_ivi_input_acceptance = state->ivi_input_acceptance;
	port->last_applied_seat_name[0] = '\0';

	if (!port->input_focus_applied ||
	    port->last_ivi_input_focus != state->ivi_input_focus) {
		ivi_input_set_input_focus(state->ivi_input,
					  port->ivi_surface_id,
					  0,
					  state->ivi_input_focus);
		port->input_focus_applied = true;
		port->last_ivi_input_focus = state->ivi_input_focus;
		focus_changed = true;
	}

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI Input Policy: surface=%u acceptance=%d seats=%d focus=%d(%s)\n",
		     port->ivi_surface_id,
		     state->ivi_input_acceptance,
		     accepted_seat_count,
		     state->ivi_input_focus,
		     focus_changed ? "applied" : "cached");
}

void
apply_ivi_input_policy(struct client_state *state)
{
	int i;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];
		if (port->port_type == PORT_TYPE_SHARED)
			apply_ivi_input_policy_for_port(state, port);
	}
}

/* --- IVI Input Listeners --- */
void
ivi_input_handle_seat_created(void *data, struct ivi_input *ivi_input,
			      const char *name, uint32_t capabilities,
			      int32_t is_default)
{
	struct client_state *state = data;
	(void) ivi_input;
	(void) capabilities;
	(void) is_default;

	snprintf(state->active_seat_name, sizeof(state->active_seat_name), "%s", name);
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI Seat Created: %s (caps=0x%x)\n",
		     name, capabilities);
	apply_ivi_input_policy(state);
}

void
ivi_input_handle_seat_capabilities(void *data, struct ivi_input *ivi_input,
				   const char *name, uint32_t capabilities)
{
	struct client_state *state = data;
	(void) ivi_input;

	snprintf(state->active_seat_name, sizeof(state->active_seat_name), "%s", name);
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI Seat Capabilities: %s (caps=0x%x)\n",
		     name, capabilities);
	apply_ivi_input_policy(state);
}

void
ivi_input_handle_seat_destroyed(void *data, struct ivi_input *ivi_input,
				const char *name)
{
	struct client_state *state = data;
	(void) ivi_input;

	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI Seat Destroyed: %s\n", name);
	if (strcmp(state->active_seat_name, name) == 0)
		state->active_seat_name[0] = '\0';
}

void
ivi_input_handle_input_focus(void *data, struct ivi_input *ivi_input,
			     uint32_t surface, uint32_t device, int32_t enabled)
{
	struct client_state *state = data;
	(void) ivi_input;

	if (!state || !state->touch_log_enabled)
		return;

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI Input Focus Event: surface=%u device=%u enabled=%d\n",
		     surface, device, enabled);
}

void
ivi_input_handle_input_acceptance(void *data, struct ivi_input *ivi_input,
				  uint32_t surface, const char *seat,
				  int32_t accepted)
{
	struct client_state *state = data;
	(void) ivi_input;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI Input Acceptance Event: surface=%u seat=%s accepted=%d\n",
		     surface, seat, accepted);
}

static const struct ivi_input_listener ivi_input_listener = {
	.seat_created = ivi_input_handle_seat_created,
	.seat_capabilities = ivi_input_handle_seat_capabilities,
	.seat_destroyed = ivi_input_handle_seat_destroyed,
	.input_focus = ivi_input_handle_input_focus,
	.input_acceptance = ivi_input_handle_input_acceptance,
};

/* --- IVI WM listeners --- */
void
ivi_wm_screen_handle_screen_id(void *data, struct ivi_wm_screen *screen,
			       uint32_t id)
{
	PortInfo *host_port = data;
	struct client_state *state = host_port ? host_port->state_ref : NULL;
	(void) screen;
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI WM Screen ID: %u\n", id);
}

void
ivi_wm_screen_handle_layer_added(void *data, struct ivi_wm_screen *screen,
				 uint32_t layer_id)
{
	PortInfo *host_port = data;
	struct client_state *state;

	(void) screen;

	if (!host_port) {
		GTP_LOG_INFO(NULL,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> IVI WM Screen Layer Added: layer=%u\n",
			     layer_id);
		return;
	}

	state = host_port->state_ref;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Screen Layer Added: host=%s layer=%u\n",
		     host_port->cport_name, layer_id);

	if (!state)
		return;

	if (state->ivi_zorder_auto) {
		if (!is_shared_layer(state, layer_id))
			track_screen_layer(host_port, layer_id);

		if (host_port->screen_layer_refresh_in_progress)
			host_port->screen_layer_refresh_in_progress = false;

		enforce_zorder_on_screen(state, host_port);
	}
}

void
ivi_wm_screen_handle_connector_name(void *data, struct ivi_wm_screen *screen,
				    const char *connector_name)
{
	PortInfo *host_port = data;
	struct client_state *state = host_port ? host_port->state_ref : NULL;
	(void) screen;
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI WM Screen Connector: %s\n",
		     connector_name);
}

void
ivi_wm_screen_handle_error(void *data, struct ivi_wm_screen *screen,
			   uint32_t error, const char *message)
{
	PortInfo *host_port = data;
	struct client_state *state = host_port ? host_port->state_ref : NULL;
	(void) screen;
	GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX "IVI WM Screen Error: code=%u msg=%s\n",
		      error, message ? message : "(null)");
}

static const struct ivi_wm_screen_listener ivi_wm_screen_listener = {
	.screen_id = ivi_wm_screen_handle_screen_id,
	.layer_added = ivi_wm_screen_handle_layer_added,
	.connector_name = ivi_wm_screen_handle_connector_name,
	.error = ivi_wm_screen_handle_error,
};

void
ivi_wm_handle_surface_visibility(void *data, struct ivi_wm *ivi_wm,
				 uint32_t surface_id, int32_t visibility)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Surface Visibility: surface=%u visibility=%d\n",
		     surface_id, visibility);
}

void
ivi_wm_handle_layer_visibility(void *data, struct ivi_wm *ivi_wm,
			       uint32_t layer_id, int32_t visibility)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Layer Visibility: layer=%u visibility=%d\n",
		     layer_id, visibility);
}

void
ivi_wm_handle_surface_opacity(void *data, struct ivi_wm *ivi_wm,
			      uint32_t surface_id, wl_fixed_t opacity)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Surface Opacity: surface=%u opacity=%f\n",
		     surface_id, wl_fixed_to_double(opacity));
}

void
ivi_wm_handle_layer_opacity(void *data, struct ivi_wm *ivi_wm,
			    uint32_t layer_id, wl_fixed_t opacity)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Layer Opacity: layer=%u opacity=%f\n",
		     layer_id, wl_fixed_to_double(opacity));
}

void
ivi_wm_handle_surface_source_rectangle(void *data, struct ivi_wm *ivi_wm,
				       uint32_t surface_id, int32_t x, int32_t y,
				       int32_t width, int32_t height)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Surface Source Rect: surface=%u (%d,%d %dx%d)\n",
		     surface_id, x, y, width, height);
}

void
ivi_wm_handle_layer_source_rectangle(void *data, struct ivi_wm *ivi_wm,
				     uint32_t layer_id, int32_t x, int32_t y,
				     int32_t width, int32_t height)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Layer Source Rect: layer=%u (%d,%d %dx%d)\n",
		     layer_id, x, y, width, height);
}

void
ivi_wm_handle_surface_destination_rectangle(void *data, struct ivi_wm *ivi_wm,
					    uint32_t surface_id, int32_t x,
					    int32_t y, int32_t width,
					    int32_t height)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Surface Dest Rect: surface=%u (%d,%d %dx%d)\n",
		     surface_id, x, y, width, height);
}

void
ivi_wm_handle_layer_destination_rectangle(void *data, struct ivi_wm *ivi_wm,
					  uint32_t layer_id, int32_t x, int32_t y,
					  int32_t width, int32_t height)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Layer Dest Rect: layer=%u (%d,%d %dx%d)\n",
		     layer_id, x, y, width, height);
}

void
ivi_wm_handle_surface_created(void *data, struct ivi_wm *ivi_wm,
			      uint32_t surface_id)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI WM Surface Created: %u\n",
		     surface_id);
}

void
ivi_wm_handle_layer_created(void *data, struct ivi_wm *ivi_wm,
			    uint32_t layer_id)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI WM Layer Created: %u\n",
		     layer_id);

	refresh_all_host_screens(state);
}

void
ivi_wm_handle_surface_destroyed(void *data, struct ivi_wm *ivi_wm,
				uint32_t surface_id)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI WM Surface Destroyed: %u\n",
		     surface_id);
}

void
ivi_wm_handle_layer_destroyed(void *data, struct ivi_wm *ivi_wm,
			      uint32_t layer_id)
{
	struct client_state *state = data;
	int i;

	(void) ivi_wm;
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI WM Layer Destroyed: %u\n",
		     layer_id);

	if (!state || !state->ivi_zorder_auto)
		return;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *host_port = &state->ports[i];

		if (host_port->port_type != PORT_TYPE_HOST)
			continue;

		untrack_screen_layer(host_port, layer_id);
	}

	refresh_all_host_screens(state);
}

void
ivi_wm_handle_surface_error(void *data, struct ivi_wm *ivi_wm,
			    uint32_t object_id, uint32_t error,
			    const char *message)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX "IVI WM Surface Error: object=%u code=%u msg=%s\n",
		      object_id, error, message ? message : "(null)");
}

void
ivi_wm_handle_layer_error(void *data, struct ivi_wm *ivi_wm,
			  uint32_t object_id, uint32_t error,
			  const char *message)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX "IVI WM Layer Error: object=%u code=%u msg=%s\n",
		      object_id, error, message ? message : "(null)");
}

void
ivi_wm_handle_surface_size(void *data, struct ivi_wm *ivi_wm,
			   uint32_t surface_id, int32_t width, int32_t height)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI WM Surface Size: surface=%u %dx%d\n",
		     surface_id, width, height);
}

void
ivi_wm_handle_surface_stats(void *data, struct ivi_wm *ivi_wm,
			    uint32_t surface_id, uint32_t frame_count,
			    uint32_t pid)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Surface Stats: surface=%u frames=%u pid=%u\n",
		     surface_id, frame_count, pid);
}

void
ivi_wm_handle_layer_surface_added(void *data, struct ivi_wm *ivi_wm,
				  uint32_t layer_id, uint32_t surface_id)
{
	struct client_state *state = data;
	(void) ivi_wm;
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM Layer Surface Added: layer=%u surface=%u\n",
		     layer_id, surface_id);
}

static const struct ivi_wm_listener ivi_wm_listener = {
	.surface_visibility = ivi_wm_handle_surface_visibility,
	.layer_visibility = ivi_wm_handle_layer_visibility,
	.surface_opacity = ivi_wm_handle_surface_opacity,
	.layer_opacity = ivi_wm_handle_layer_opacity,
	.surface_source_rectangle = ivi_wm_handle_surface_source_rectangle,
	.layer_source_rectangle = ivi_wm_handle_layer_source_rectangle,
	.surface_destination_rectangle = ivi_wm_handle_surface_destination_rectangle,
	.layer_destination_rectangle = ivi_wm_handle_layer_destination_rectangle,
	.surface_created = ivi_wm_handle_surface_created,
	.layer_created = ivi_wm_handle_layer_created,
	.surface_destroyed = ivi_wm_handle_surface_destroyed,
	.layer_destroyed = ivi_wm_handle_layer_destroyed,
	.surface_error = ivi_wm_handle_surface_error,
	.layer_error = ivi_wm_handle_layer_error,
	.surface_size = ivi_wm_handle_surface_size,
	.surface_stats = ivi_wm_handle_surface_stats,
	.layer_surface_added = ivi_wm_handle_layer_surface_added,
};

/* --- Touch Handling --- */
void
touch_down(void *data, struct wl_touch *wl_touch, uint32_t serial,
	   uint32_t time, struct wl_surface *surface, int32_t id,
	   wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct gtp_seat *seat = data;
	struct client_state *state = seat ? seat->state : NULL;
	PortInfo *port = find_port_by_surface(state, surface);
	int x;
	int y;
	int slot;

	(void) wl_touch;
	(void) serial;
	(void) time;

	if (!port || port->uinput_fd < 0)
		return;

	if (!allocate_touch_route(state, seat, id, port)) {
		GTP_LOG_ERROR(state,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: no free touch route for seat=%s id=%d port=%s\n",
			      (seat && seat->name[0]) ? seat->name : "<unnamed>",
			      id, port->cport_name[0] ? port->cport_name : port->output_name);
		return;
	}

	x = wl_fixed_to_int(x_w);
	y = wl_fixed_to_int(y_w);
	slot = id % MAX_TOUCH_SLOTS;

	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_SLOT, slot);
	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_TRACKING_ID, id);
	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_POSITION_X, x);
	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_POSITION_Y, y);

	if (port->active_touches == 0)
		emit_touch_event(port->uinput_fd, EV_KEY, BTN_TOUCH, 1);

	port->active_touches++;
	emit_touch_event(port->uinput_fd, EV_SYN, SYN_REPORT, 0);

	gtp_log_touch_raw_coordinates(state, port, "DOWN", id, x_w, y_w);
	gtp_log_touch_event(state, port, "DOWN", id, x, y);
}

void
touch_up(void *data, struct wl_touch *wl_touch, uint32_t serial,
	 uint32_t time, int32_t id)
{
	struct gtp_seat *seat = data;
	struct client_state *state = seat ? seat->state : NULL;
	struct touch_route *route;
	PortInfo *port;
	int slot;
	(void) wl_touch;
	(void) serial;
	(void) time;

	route = find_touch_route_by_id(state, seat, id);
	if (!route || !route->port)
		return;

	port = route->port;
	if (port->uinput_fd < 0 || port->active_touches == 0) {
		clear_touch_route(route);
		return;
	}

	slot = id % MAX_TOUCH_SLOTS;
	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_SLOT, slot);
	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);

	port->active_touches--;
	if (port->active_touches <= 0) {
		port->active_touches = 0;
		emit_touch_event(port->uinput_fd, EV_KEY, BTN_TOUCH, 0);
	}
	emit_touch_event(port->uinput_fd, EV_SYN, SYN_REPORT, 0);

	gtp_log_touch_event(state, port, "UP", id, -1, -1);
	clear_touch_route(route);
}

void
touch_motion(void *data, struct wl_touch *wl_touch, uint32_t time,
	     int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct gtp_seat *seat = data;
	struct client_state *state = seat ? seat->state : NULL;
	struct touch_route *route;
	PortInfo *port;
	int x;
	int y;
	int slot;
	(void) wl_touch;
	(void) time;

	route = find_touch_route_by_id(state, seat, id);
	if (!route || !route->port)
		return;

	port = route->port;
	if (port->uinput_fd < 0 || port->active_touches == 0)
		return;

	x = wl_fixed_to_int(x_w);
	y = wl_fixed_to_int(y_w);
	slot = id % MAX_TOUCH_SLOTS;

	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_SLOT, slot);
	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_POSITION_X, x);
	emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_POSITION_Y, y);
	emit_touch_event(port->uinput_fd, EV_SYN, SYN_REPORT, 0);

	gtp_log_touch_raw_coordinates(state, port, "MOTION", id, x_w, y_w);
	gtp_log_touch_event(state, port, "MOTION", id, x, y);
}

void
touch_frame(void *data, struct wl_touch *wl_touch)
{
	(void) data;
	(void) wl_touch;
}

void
touch_cancel(void *data, struct wl_touch *wl_touch)
{
	struct gtp_seat *seat = data;
	struct client_state *state = seat ? seat->state : NULL;
	int i;
	(void) wl_touch;

	for (i = 0; i < MAX_TOUCH_SLOTS; i++) {
		struct touch_route *route = &state->touch_routes[i];
		PortInfo *port;

		if (!route->active || !route->port)
			continue;

		if (route->seat != seat)
			continue;

		port = route->port;
		if (port->uinput_fd >= 0 && port->active_touches > 0) {
			int slot = route->id % MAX_TOUCH_SLOTS;

			emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_SLOT, slot);
			emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
			port->active_touches--;
			if (port->active_touches <= 0) {
				port->active_touches = 0;
				emit_touch_event(port->uinput_fd, EV_KEY, BTN_TOUCH, 0);
			}
			emit_touch_event(port->uinput_fd, EV_SYN, SYN_REPORT, 0);
			gtp_log_touch_event(state, port, "CANCEL", route->id, -1, -1);
		}
		clear_touch_route(route);
	}
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
};

static void
seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
			 uint32_t capabilities)
{
	struct gtp_seat *seat = data;
	struct client_state *state;

	if (!seat)
		return;

	state = seat->state;
	if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !seat->touch) {
		seat->touch = wl_seat_get_touch(wl_seat);
		if (!seat->touch)
			return;

		wl_touch_add_listener(seat->touch, &touch_listener, seat);
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> Seat touch enabled: registry=%u seat=%s\n",
			     seat->registry_name,
			     seat->name[0] ? seat->name : "<unnamed>");
		apply_ivi_input_policy(state);
	} else if (!(capabilities & WL_SEAT_CAPABILITY_TOUCH) && seat->touch) {
		wl_touch_destroy(seat->touch);
		seat->touch = NULL;
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> Seat touch disabled: registry=%u seat=%s\n",
			     seat->registry_name,
			     seat->name[0] ? seat->name : "<unnamed>");
	}
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
	struct gtp_seat *seat = data;
	struct client_state *state;
	(void) wl_seat;

	if (!seat)
		return;

	state = seat->state;
	snprintf(seat->name, sizeof(seat->name), "%s", name ? name : "");

	if (seat->name[0] != '\0') {
		snprintf(state->active_seat_name, sizeof(state->active_seat_name),
			 "%s", seat->name);
		state->active_seat_name[sizeof(state->active_seat_name) - 1] = '\0';
	}

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> Seat name announced: registry=%u seat=%s\n",
		     seat->registry_name,
		     seat->name[0] ? seat->name : "<unnamed>");
	apply_ivi_input_policy(state);
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name
};

static int
create_shm_file(off_t size)
{
	int fd = -1;

	if (size <= 0) {
		GTP_LOG_ERROR(NULL, GVM_TOUCH_LOG_PREFIX "Error: invalid shm size %ld\n",
			      (long) size);
		return -1;
	}

#ifdef SYS_memfd_create
	fd = syscall(SYS_memfd_create, "gvm-touch-proxy",
		     MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd >= 0) {
		if (ftruncate(fd, size) == 0) {
			GTP_LOG_ERROR(NULL,
				      GVM_TOUCH_LOG_PREFIX
				      "SHM memfd created: fd=%d size=%ld\n",
				      fd, (long) size);
			return fd;
		}

		GTP_LOG_ERROR(NULL,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: ftruncate(%ld) failed for memfd: %s\n",
			      (long) size, strerror(errno));
		close(fd);
		fd = -1;
	}
#endif

	{
		char shm_name[64];

		snprintf(shm_name, sizeof(shm_name), "/gvm-touch-proxy-%d-%ld",
			 getpid(), random());

		fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd < 0) {
			GTP_LOG_ERROR(NULL,
				      GVM_TOUCH_LOG_PREFIX
				      "Error: shm_open failed for %s: %s\n",
				      shm_name, strerror(errno));
			return -1;
		}

		shm_unlink(shm_name);

		if (ftruncate(fd, size) < 0) {
			GTP_LOG_ERROR(NULL,
				      GVM_TOUCH_LOG_PREFIX
				      "Error: ftruncate(%ld) failed for shm object: %s\n",
				      (long) size, strerror(errno));
			close(fd);
			return -1;
		}

		GTP_LOG_ERROR(NULL,
			      GVM_TOUCH_LOG_PREFIX
			      "SHM object created: name=%s fd=%d size=%ld\n",
			      shm_name, fd, (long) size);
	}

	return fd;
}

static void
destroy_port_buffer(PortInfo *port)
{
	if (!port)
		return;

	if (port->buffer) {
		wl_buffer_destroy(port->buffer);
		port->buffer = NULL;
	}

	if (port->buffer_data) {
		munmap(port->buffer_data, port->buffer_size);
		port->buffer_data = NULL;
	}

	if (port->buffer_fd >= 0) {
		close(port->buffer_fd);
		port->buffer_fd = -1;
	}

	port->buffer_stride = 0;
	port->buffer_size = 0;
}

static bool
create_port_buffer(struct client_state *state, PortInfo *port)
{
	struct wl_shm_pool *pool = NULL;
	int width;
	int height;
	int stride;
	int size;
	int fd;
	void *data;

	if (!state || !state->shm || !port)
		return false;

	if (port->buffer)
		return true;

	width = port->width > 0 ? port->width : 1;
	height = port->height > 0 ? port->height : 1;
	stride = width * 4;
	size = stride * height;

	fd = create_shm_file(size);
	if (fd < 0) {
		GTP_LOG_ERROR(state,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: failed to create shm file for port %s (%dx%d)\n",
			      port->cport_name[0] ? port->cport_name : port->output_name,
			      width, height);
		return false;
	}

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		GTP_LOG_ERROR(state,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: mmap failed for port %s buffer: %s\n",
			      port->cport_name[0] ? port->cport_name : port->output_name,
			      strerror(errno));
		close(fd);
		return false;
	}

	memset(data, 0, size);

	pool = wl_shm_create_pool(state->shm, fd, size);
	if (!pool) {
		GTP_LOG_ERROR(state,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: wl_shm_create_pool failed for port %s\n",
			      port->cport_name[0] ? port->cport_name : port->output_name);
		munmap(data, size);
		close(fd);
		return false;
	}

	port->buffer = wl_shm_pool_create_buffer(pool, 0,
						 width, height,
						 stride,
						 WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);

	if (!port->buffer) {
		GTP_LOG_ERROR(state,
			      GVM_TOUCH_LOG_PREFIX
			      "Error: failed to create wl_buffer for port %s (%dx%d)\n",
			      port->cport_name[0] ? port->cport_name : port->output_name,
			      width, height);
		munmap(data, size);
		close(fd);
		return false;
	}

	port->buffer_fd = fd;
	port->buffer_data = data;
	port->buffer_stride = stride;
	port->buffer_size = size;

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> Created static buffer for port %s (%dx%d stride=%d size=%d)\n",
		     port->cport_name[0] ? port->cport_name : port->output_name,
		     width, height, stride, size);
	return true;
}

static void
gtp_reset_touch_runtime_state(struct client_state *state, bool emit_release)
{
	int i;

	if (!state)
		return;

	for (i = 0; i < MAX_TOUCH_SLOTS; i++) {
		struct touch_route *route = &state->touch_routes[i];
		PortInfo *port = route->port;

		if (emit_release && route->active && port &&
		    port->uinput_fd >= 0 && port->active_touches > 0) {
			int slot = route->id % MAX_TOUCH_SLOTS;

			emit_touch_event(port->uinput_fd, EV_ABS, ABS_MT_SLOT, slot);
			emit_touch_event(port->uinput_fd, EV_ABS,
					 ABS_MT_TRACKING_ID, -1);
		}

		clear_touch_route(route);
	}

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		if (emit_release && port->uinput_fd >= 0 &&
		    port->active_touches > 0) {
			emit_touch_event(port->uinput_fd, EV_KEY, BTN_TOUCH, 0);
			emit_touch_event(port->uinput_fd, EV_SYN, SYN_REPORT, 0);
		}

		port->active_touches = 0;
	}
}

static void
gtp_wayland_session_teardown(struct client_state *state)
{
	int i;

	if (!state)
		return;

	gtp_reset_touch_runtime_state(state, true);

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		if (port->ivi_wm_screen) {
			ivi_wm_screen_destroy(port->ivi_wm_screen);
			port->ivi_wm_screen = NULL;
		}

		free(port->screen_layer_ids);
		port->screen_layer_ids = NULL;
		port->screen_layer_count = 0;
		port->screen_layer_capacity = 0;

		free(port->ordered_layer_ids);
		port->ordered_layer_ids = NULL;
		port->ordered_layer_capacity = 0;

		port->screen_layer_refresh_in_progress = false;
		port->state_ref = NULL;
		port->input_acceptance_applied = false;
		port->last_ivi_input_acceptance = 0;
		port->input_focus_applied = false;
		port->last_ivi_input_focus = 0;
		port->last_applied_seat_name[0] = '\0';

		destroy_port_buffer(port);

		if (port->xdg_toplevel) {
			xdg_toplevel_destroy(port->xdg_toplevel);
			port->xdg_toplevel = NULL;
		}

		if (port->xdg_surface) {
			xdg_surface_destroy(port->xdg_surface);
			port->xdg_surface = NULL;
		}

		port->xdg_configured = false;

		if (port->ivi_surface) {
			ivi_surface_destroy(port->ivi_surface);
			port->ivi_surface = NULL;
		}

		if (port->surface) {
			wl_surface_destroy(port->surface);
			port->surface = NULL;
		}
	}

	destroy_all_seats(state);

	if (state->ivi_input) {
		ivi_input_destroy(state->ivi_input);
		state->ivi_input = NULL;
	}

	if (state->ivi_wm) {
		ivi_wm_destroy(state->ivi_wm);
		state->ivi_wm = NULL;
	}

	if (state->ivi_app) {
		ivi_application_destroy(state->ivi_app);
		state->ivi_app = NULL;
	}

	if (state->xdg_wm_base) {
		xdg_wm_base_destroy(state->xdg_wm_base);
		state->xdg_wm_base = NULL;
	}

	if (state->shm) {
		wl_shm_destroy(state->shm);
		state->shm = NULL;
	}

	if (state->compositor) {
		wl_compositor_destroy(state->compositor);
		state->compositor = NULL;
	}

	for (i = 0; i < state->output_count; i++) {
		if (state->outputs[i].output) {
			wl_output_destroy(state->outputs[i].output);
			state->outputs[i].output = NULL;
		}

		state->outputs[i].registry_name = 0;
		state->outputs[i].bound_version = 0;
		state->outputs[i].name[0] = '\0';
		state->outputs[i].width = 0;
		state->outputs[i].height = 0;
	}
	state->output_count = 0;

	if (state->registry) {
		wl_registry_destroy(state->registry);
		state->registry = NULL;
	}

#if GVM_TOUCH_PROXY_ENABLE_TOUCH_ONLY_SURFACE
	if (state->touch_only) {
		touch_only_surface_destroy(state->touch_only);
		state->touch_only = NULL;
	}
#endif

	if (state->display) {
		wl_display_disconnect(state->display);
		state->display = NULL;
	}

	state->use_ivi_shell = false;
	state->active_seat_name[0] = '\0';
}

bool
gtp_wayland_session_init(struct client_state *state)
{
	if (!state)
		return false;

	state->display = connect_to_wayland_display_with_retry();
	if (!state->display) {
		GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX
			      "Failed to connect to wayland display\n");
		return false;
	}

	state->registry = wl_display_get_registry(state->display);
	if (!state->registry) {
		GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX
			      "Failed to get wayland registry\n");
		gtp_wayland_session_teardown(state);
		return false;
	}

	wl_registry_add_listener(state->registry, &registry_listener, state);

	if (wl_display_roundtrip(state->display) == -1 ||
	    wl_display_roundtrip(state->display) == -1) {
		GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX
			      "Initial registry roundtrip failed: %s\n",
			      strerror(errno));
		gtp_wayland_session_teardown(state);
		return false;
	}

	create_surfaces(state);

	if (wl_display_roundtrip(state->display) == -1) {
		GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX
			      "Surface setup roundtrip failed: %s\n",
			      strerror(errno));
		gtp_wayland_session_teardown(state);
		return false;
	}

	if (state->ivi_window_management) {
		setup_ivi_window_management(state);

		if (wl_display_roundtrip(state->display) == -1) {
			GTP_LOG_ERROR(state, GVM_TOUCH_LOG_PREFIX
				      "IVI WM setup roundtrip failed: %s\n",
				      strerror(errno));
			gtp_wayland_session_teardown(state);
			return false;
		}
	}

	GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX
		     ">>> Wayland session initialized successfully\n");
	return true;
}

void
gtp_final_cleanup(struct client_state *state)
{
	int i;

	if (!state)
		return;

	gtp_wayland_session_teardown(state);

	for (i = 0; i < state->port_count; i++) {
		if (state->ports[i].uinput_fd >= 0) {
			ioctl(state->ports[i].uinput_fd, UI_DEV_DESTROY);
			close(state->ports[i].uinput_fd);
			state->ports[i].uinput_fd = -1;
		}
	}

	gtp_close_log_output(state);
}

/* --- Output Listeners --- */
void
output_handle_mode(void *data, struct wl_output *wl_output,
		   uint32_t flags, int width, int height, int refresh)
{
	struct client_state *state = data;
	int i;

	(void) refresh;

	for (i = 0; i < state->output_count; i++) {
		if (state->outputs[i].output == wl_output &&
		    (flags & WL_OUTPUT_MODE_CURRENT)) {
			state->outputs[i].width = width;
			state->outputs[i].height = height;
			GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> Output %s: %dx%d\n",
				     state->outputs[i].name, width, height);
		}
	}
}

void
output_handle_name(void *data, struct wl_output *wl_output, const char *name)
{
	struct client_state *state = data;

	int i;

	for (i = 0; i < state->output_count; i++) {
		if (state->outputs[i].output != wl_output)
			continue;

		snprintf(state->outputs[i].name, sizeof(state->outputs[i].name),
			 "%s", name);
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> Found Output: %s (registry=%u version=%u)\n",
			     name, state->outputs[i].registry_name,
			     state->outputs[i].bound_version);
		return;
	}
}

static void
output_handle_description(void *data, struct wl_output *wl_output,
			  const char *description)
{
	(void) data;
	(void) wl_output;
	(void) description;
}

static void
output_handle_geometry(void *data, struct wl_output *wl_output, int x, int y,
		       int phys_w, int phys_h, int subpixel,
		       const char *make, const char *model, int transform)
{
	(void) data;
	(void) wl_output;
	(void) x;
	(void) y;
	(void) phys_w;
	(void) phys_h;
	(void) subpixel;
	(void) make;
	(void) model;
	(void) transform;
}

static void
output_handle_done(void *data, struct wl_output *wl_output)
{
	(void) data;
	(void) wl_output;
}

static void
output_handle_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
	(void) data;
	(void) wl_output;
	(void) factor;
}

static const struct wl_output_listener output_listener = {
	.geometry = output_handle_geometry,
	.mode = output_handle_mode,
	.done = output_handle_done,
	.scale = output_handle_scale,
	.name = output_handle_name,
	.description = output_handle_description,
};

/* --- Registry --- */
static void
registry_handle_global(void *data, struct wl_registry *registry,
		       uint32_t name, const char *interface,
		       uint32_t version)
{
	struct client_state *state = data;

	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> Global discovered: %s name=%u version=%u\n",
		     interface, name, version);

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		uint32_t bind_version = MIN_U32(version, 1);
		state->compositor = wl_registry_bind(registry, name,
						     &wl_compositor_interface, bind_version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		uint32_t bind_version = MIN_U32(version, 1);
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, bind_version);
	} else if (strcmp(interface, "ivi_application") == 0) {
		uint32_t bind_version = MIN_U32(version, 1);
		state->ivi_app = wl_registry_bind(registry, name,
						  &ivi_application_interface, bind_version);
		state->use_ivi_shell = true;
		GTP_LOG_INFO(state, GVM_TOUCH_LOG_PREFIX ">>> IVI-Shell Support Detected.\n");
	} else if (strcmp(interface, "xdg_wm_base") == 0) {
		uint32_t bind_version = MIN_U32(version, 1);
		state->xdg_wm_base = wl_registry_bind(registry, name,
							   &xdg_wm_base_interface,
							   bind_version);
		xdg_wm_base_add_listener(state->xdg_wm_base,
					 &xdg_wm_base_listener, state);
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> XDG WM Base bound (version=%u)\n",
			     bind_version);
	} else if (strcmp(interface, "ivi_input") == 0) {
		uint32_t bind_version = MIN_U32(version, 2);
		state->ivi_input = wl_registry_bind(registry, name,
						    &ivi_input_interface, bind_version);
		ivi_input_add_listener(state->ivi_input, &ivi_input_listener, state);
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> IVI Input Support Detected (bind_version=%u).\n",
			     bind_version);
	} else if (strcmp(interface, "ivi_wm") == 0) {
		uint32_t bind_version = MIN_U32(version, 2);
		state->ivi_wm = wl_registry_bind(registry, name,
						 &ivi_wm_interface, bind_version);
		ivi_wm_add_listener(state->ivi_wm, &ivi_wm_listener, state);
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> IVI Window Management Support Detected (bind_version=%u).\n",
			     bind_version);

#if GVM_TOUCH_PROXY_ENABLE_TOUCH_ONLY_SURFACE
	} else if (strcmp(interface, "touch_only_surface") == 0) {
		uint32_t bind_version = MIN_U32(version, 1);
		state->touch_only =
			wl_registry_bind(registry, name,
					 &touch_only_surface_interface,
					 bind_version);
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> touch_only_surface bound (version=%u)\n",
			     bind_version);
#else
	} else if (strcmp(interface, "touch_only_surface") == 0) {
		uint32_t bind_version = MIN_U32(version, 1);
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> touch_only_surface found but disabled by macro (version=%u)\n",
			     bind_version);
#endif
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct gtp_seat *seat = add_seat(state, registry, name, version);

		if (!seat) {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX
				      "Error: failed to bind wl_seat name=%u\n",
				      name);
			return;
		}

		wl_seat_add_listener(seat->seat, &seat_listener, seat);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		uint32_t bind_version = MIN_U32(version, 4);

		if (state->output_count < MAX_OUTPUTS) {
			OutputInfo *out = &state->outputs[state->output_count];

			memset(out, 0, sizeof(*out));
			out->registry_name = name;
			out->bound_version = bind_version;
			out->output = wl_registry_bind(registry, name,
						      &wl_output_interface, bind_version);
			wl_output_add_listener(out->output, &output_listener, state);
			state->output_count++;
			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> Bound wl_output name=%u bind_version=%u\n",
				     name, bind_version);
		}
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
			      uint32_t name)
{
	struct client_state *state = data;
	struct gtp_seat *seat;
	struct gtp_seat *prev = NULL;
	(void) registry;

	if (!state)
		return;

	for (seat = state->seats; seat; prev = seat, seat = seat->next) {
		if (seat->registry_name != name)
			continue;

		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> Seat removed: registry=%u seat=%s\n",
			     name, seat->name[0] ? seat->name : "<unnamed>");

		if (seat->touch) {
			wl_touch_destroy(seat->touch);
			seat->touch = NULL;
		}

		if (seat->seat) {
			wl_seat_destroy(seat->seat);
			seat->seat = NULL;
		}

		if (prev)
			prev->next = seat->next;
		else
			state->seats = seat->next;

		if (state->active_seat_name[0] != '\0' &&
		    strcmp(state->active_seat_name, seat->name) == 0) {
			struct gtp_seat *replacement = state->seats;

			if (replacement && replacement->name[0] != '\0') {
				snprintf(state->active_seat_name,
					 sizeof(state->active_seat_name),
					 "%s", replacement->name);
			} else {
				state->active_seat_name[0] = '\0';
			}
		}

		free(seat);
		return;
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove
};

static void
setup_ivi_window_management(struct client_state *state)
{
	int i;

	if (!state->ivi_wm || !state->ivi_window_management) {
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> IVI WM setup skipped (available=%s, enabled=%s)\n",
			     state->ivi_wm ? "yes" : "no",
			     state->ivi_window_management ? "yes" : "no");
		return;
	}

	for (i = 0; i < state->port_count; i++) {
		PortInfo *host_port = &state->ports[i];
		PortInfo *shared_ports[MAX_PORTS];
		int shared_count = 0;
		OutputInfo *out;

		if (host_port->port_type != PORT_TYPE_HOST)
			continue;

		out = find_output_for_port(state, host_port);
		if (!out || !out->output) {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX
				      "Warning: no wl_output found for host port %s (display_node=%u display_id=%u), skip IVI WM screen\n",
				      host_port->cport_name, host_port->display_node,
				      host_port->display_id);
			continue;
		}

		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> IVI WM host port %s mapped to wl_output name=%s registry=%u version=%u\n",
			     host_port->cport_name,
			     out->name[0] ? out->name : "<unnamed>",
			     out->registry_name, out->bound_version);

		host_port->state_ref = state;
		host_port->screen_layer_count = 0;
		host_port->screen_layer_capacity = 0;
		host_port->screen_layer_ids = NULL;
		host_port->screen_layer_refresh_in_progress = false;
		host_port->ordered_layer_capacity = 0;
		host_port->ordered_layer_ids = NULL;
		host_port->ivi_wm_screen =
			ivi_wm_create_screen(state->ivi_wm, out->output);
		if (!host_port->ivi_wm_screen) {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX
				      "Error: ivi_wm_create_screen returned NULL for host port %s\n",
				      host_port->cport_name);
			continue;
		}
		ivi_wm_screen_add_listener(host_port->ivi_wm_screen,
					   &ivi_wm_screen_listener, host_port);
		GTP_LOG_INFO(state,
			     GVM_TOUCH_LOG_PREFIX
			     ">>> Created IVI WM screen for host port %s\n",
			     host_port->cport_name);

		for (int j = 0; j < state->port_count; j++) {
			PortInfo *sp = &state->ports[j];
			if (sp->port_type == PORT_TYPE_SHARED && sp->host_port == host_port)
				shared_ports[shared_count++] = sp;
		}

		qsort(shared_ports, shared_count, sizeof(shared_ports[0]),
		      gtp_compare_shared_ports_by_zorder);

		for (int j = 0; j < shared_count; j++) {
			PortInfo *sp = shared_ports[j];

			if (sp->ivi_layer_id == 0 || sp->ivi_surface_id == 0) {
				GTP_LOG_ERROR(state,
					      GVM_TOUCH_LOG_PREFIX
					      "Warning: skip shared port %s due to missing layer/surface id\n",
					      sp->cport_name);
				continue;
			}

			gtp_resolve_port_geometry(state, sp);

			ivi_wm_create_layout_layer(state->ivi_wm,
						   sp->ivi_layer_id,
						   sp->width,
						   sp->height);
			ivi_wm_set_layer_visibility(state->ivi_wm,
						    sp->ivi_layer_id, 1);
			ivi_wm_set_layer_destination_rectangle(state->ivi_wm,
							       sp->ivi_layer_id,
							       0, 0,
							       host_port->width,
							       host_port->height);

			ivi_wm_screen_add_layer(host_port->ivi_wm_screen,
						sp->ivi_layer_id);

			ivi_wm_layer_add_surface(state->ivi_wm,
						 sp->ivi_layer_id,
						 sp->ivi_surface_id);
			ivi_wm_set_surface_visibility(state->ivi_wm,
						      sp->ivi_surface_id, 1);
			ivi_wm_set_surface_destination_rectangle(state->ivi_wm,
								 sp->ivi_surface_id,
								 0, 0,
								 sp->width,
								 sp->height);
			ivi_wm_set_surface_type(state->ivi_wm,
						sp->ivi_surface_id,
						IVI_WM_SURFACE_TYPE_DESKTOP);

			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> IVI WM: host=%s layer=%u surface=%u z=%d\n",
				     host_port->cport_name,
				     sp->ivi_layer_id,
				     sp->ivi_surface_id,
				     sp->zorder_base);
		}

		if (state->ivi_zorder_auto)
			refresh_screen_layer_order(host_port);
	}

	if (state->display)
		wl_display_roundtrip(state->display);

	for (i = 0; i < state->port_count; i++) {
		PortInfo *host_port = &state->ports[i];

		if (host_port->port_type != PORT_TYPE_HOST)
			continue;

		if (state->ivi_zorder_auto)
			enforce_zorder_on_screen(state, host_port);
	}

	ivi_wm_commit_changes(state->ivi_wm);
	GTP_LOG_INFO(state,
		     GVM_TOUCH_LOG_PREFIX
		     ">>> IVI WM: committed window management changes\n");
}

/* --- Create Surfaces --- */
static void
xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface,
			      uint32_t serial)
{
	PortInfo *port = data;
	struct client_state *state = port ? port->state_ref : NULL;

	xdg_surface_ack_configure(xdg_surface, serial);

	if (!port->xdg_configured) {
		port->xdg_configured = true;
		if (port->buffer && port->surface) {
			wl_surface_attach(port->surface, port->buffer, 0, 0);
			wl_surface_commit(port->surface);
			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> XDG Surface configured and committed: %s\n",
				     port->cport_name);
		}
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void
create_surfaces(struct client_state *state)
{
	int i;

	for (i = 0; i < state->port_count; i++) {
		PortInfo *port = &state->ports[i];

		port->state_ref = state;
		gtp_resolve_port_geometry(state, port);

		/* HOST ports have no surface in any mode:
		 * - IVI-shell: managed via ivi_wm screen/layer only
		 * - XDG: no shell role assigned to gvm-touch-proxy in production
		 */
		if (port->port_type == PORT_TYPE_HOST)
			continue;

		/* SHARED ports only from here */
		port->surface = wl_compositor_create_surface(state->compositor);

		/* Input region covers the shared port's full area. */
		{
			struct wl_region *input_region =
				wl_compositor_create_region(state->compositor);

			if (input_region) {
				wl_region_add(input_region,
					      0, 0,
					      port->width, port->height);
				wl_surface_set_input_region(port->surface,
							    input_region);
				wl_region_destroy(input_region);
				GTP_LOG_INFO(state,
					     GVM_TOUCH_LOG_PREFIX
					     ">>> Set input region for shared port: %s (%dx%d)\n",
					     port->cport_name,
					     port->width, port->height);
			} else {
				GTP_LOG_ERROR(state,
					      GVM_TOUCH_LOG_PREFIX
					      "Error: failed to create input region for %s\n",
					      port->cport_name);
			}
		}

#if GVM_TOUCH_PROXY_ENABLE_TOUCH_ONLY_SURFACE
		if (state->touch_only) {
			touch_only_surface_set_touch_only(state->touch_only,
							  port->surface, 1);
			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> touch_only_surface set for shared port %s\n",
				     port->cport_name);
		}
#endif

		if (!create_port_buffer(state, port)) {
			GTP_LOG_ERROR(state,
				      GVM_TOUCH_LOG_PREFIX
				      "Error: failed to create static buffer for %s\n",
				      port->cport_name[0] ? port->cport_name
							  : port->output_name);
			wl_surface_destroy(port->surface);
			port->surface = NULL;
			continue;
		}

		if (state->use_ivi_shell && port->ivi_surface_id > 0) {
			/* IVI-shell path */
			port->ivi_surface = ivi_application_surface_create(
				state->ivi_app, port->ivi_surface_id,
				port->surface);
			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> Created IVI Surface: %s (ID=%u, %dx%d)\n",
				     port->cport_name, port->ivi_surface_id,
				     port->width, port->height);

			wl_surface_attach(port->surface, port->buffer, 0, 0);
			wl_surface_commit(port->surface);

			if (state->ivi_wm) {
				ivi_wm_set_surface_source_rectangle(
					state->ivi_wm, port->ivi_surface_id,
					0, 0, port->width, port->height);
			}

			apply_ivi_input_policy_for_port(state, port);
		} else {
			/* XDG path: create xdg_toplevel so the surface enters
			 * the weston scene graph and participates in hit-testing.
			 * Must not attach buffer until xdg_surface.configure is received. */
			if (state->xdg_wm_base) {
				port->xdg_surface =
					xdg_wm_base_get_xdg_surface(
						state->xdg_wm_base,
						port->surface);
				if (port->xdg_surface) {
					xdg_surface_add_listener(
						port->xdg_surface,
						&xdg_surface_listener, port);
					port->xdg_toplevel =
						xdg_surface_get_toplevel(
							port->xdg_surface);
					xdg_toplevel_set_title(
						port->xdg_toplevel,
						port->cport_name);
				}
			}

			/* Initial commit with no buffer triggers the configure event. */
			wl_surface_commit(port->surface);
			GTP_LOG_INFO(state,
				     GVM_TOUCH_LOG_PREFIX
				     ">>> Created XDG Surface (shared): %s (%dx%d)\n",
				     port->cport_name, port->width, port->height);
		}
	}

	if (state->display)
		wl_display_flush(state->display);
}

/* --- Main --- */
#ifndef GVM_TOUCH_PROXY_UNIT_TEST
static void
sigterm_handler(int signo)
{
	(void) signo;
	gvm_touch_running = 0;
}

int
main(int argc, char **argv)
{
	struct client_state state;
	char ini_config_path[256];
	char xml_config_path[256];
	const char *ini_path;
	const char *xml_path;
	(void) argc;
	(void) argv;

	signal(SIGTERM, sigterm_handler);
	signal(SIGINT,  sigterm_handler);

	memset(&state, 0, sizeof(state));
	state.ivi_window_management = true;
	state.ivi_zorder_auto = true;
	state.ivi_input_acceptance = 1;
	state.ivi_input_focus = 1;
	state.touch_log_enabled = false;
	state.log_control_mode = GVM_TOUCH_LOG_CONTROL_STDOUT;
	state.log_fp = NULL;

	LIBXML_TEST_VERSION

	ini_path = resolve_ini_config_path(ini_config_path,
					   sizeof(ini_config_path));
	if (!ini_path) {
		GTP_LOG_ERROR(&state, GVM_TOUCH_LOG_PREFIX "Error: Failed to resolve shared_touch.ini path. Exiting.\n");
		return -1;
	}

	GTP_LOG_INFO(&state, GVM_TOUCH_LOG_PREFIX ">>> INI Config Path: %s\n", ini_path);

	if (!gtp_parse_ini_file(ini_path, &state)) {
		GTP_LOG_ERROR(&state, GVM_TOUCH_LOG_PREFIX "Error: Failed to parse shared_touch.ini from %s. Exiting.\n",
			ini_path);
		return -1;
	}

	gtp_init_log_output(&state);

	xml_path = resolve_xml_config_path(xml_config_path,
					   sizeof(xml_config_path));
	if (!xml_path) {
		GTP_LOG_ERROR(&state, GVM_TOUCH_LOG_PREFIX "Error: Failed to resolve qcdisplaycfg.xml path. Exiting.\n");
		return -1;
	}

	GTP_LOG_INFO(&state, GVM_TOUCH_LOG_PREFIX ">>> XML Config Path: %s\n", xml_path);

	if (!parse_xml_config(xml_path, &state)) {
		GTP_LOG_ERROR(&state, GVM_TOUCH_LOG_PREFIX "Error: Failed to parse qcdisplaycfg.xml from %s. Exiting.\n",
			xml_path);
		return -1;
	}

	gtp_link_ports(&state);

	if (!gtp_validate_shared_port_configuration(&state))
		return -1;

	calculate_zorder(&state);

	if (!setup_shared_port_uinputs(&state))
		return -1;

	while (gvm_touch_running) {
		int dispatch_ret;
		int err;

		if (!gtp_wayland_session_init(&state)) {
			if (!gvm_touch_running)
				break;

			GTP_LOG_ERROR(&state,
				      GVM_TOUCH_LOG_PREFIX
				      "Wayland session init failed, retry in 1s\n");
			sleep(GVM_TOUCH_WAYLAND_RETRY_INTERVAL_S);
			continue;
		}

		GTP_LOG_INFO(&state, GVM_TOUCH_LOG_PREFIX
			     ">>> GVM Touch Proxy running with %d ports...\n",
			     state.port_count);

		while (gvm_touch_running) {
			dispatch_ret = wl_display_dispatch(state.display);
			if (dispatch_ret != -1)
				continue;

			err = wl_display_get_error(state.display);
			GTP_LOG_ERROR(&state,
				      GVM_TOUCH_LOG_PREFIX
				      "Wayland session lost: errno=%d (%s)\n",
				      err, strerror(err));
			break;
		}

		gtp_wayland_session_teardown(&state);

		if (gvm_touch_running)
			sleep(GVM_TOUCH_WAYLAND_RETRY_INTERVAL_S);
	}

	gtp_final_cleanup(&state);
	xmlCleanupParser();
	return 0;
}
#endif
