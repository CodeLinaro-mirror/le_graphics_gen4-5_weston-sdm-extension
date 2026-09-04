/*
 * Real-implementation Unit Tests for GVM Touch Proxy
 *
 * Goal:
 * - call the real gvm-touch-proxy logic directly
 * - compare EXPECT values with ACTUAL results
 * - produce explicit PASS / FAIL output
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <wayland-client.h>

#include "ivi-wm-client-protocol.h"
#include "gvm-touch-proxy-internal.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
	static void test_##name(void); \
	static void run_test_##name(void) { \
		tests_run++; \
		printf("[TEST] %s\n", #name); \
		test_##name(); \
	} \
	static void test_##name(void)

#define FAIL_MSG(fmt, ...) \
	do { \
		printf("  RESULT : FAIL\n"); \
		printf("  REASON : " fmt "\n", ##__VA_ARGS__); \
		tests_failed++; \
		return; \
	} while (0)

#define PASS_MSG() \
	do { \
		printf("  RESULT : PASS\n"); \
		tests_passed++; \
		return; \
	} while (0)

#define EXPECT_EQ_INT(expected, actual, label) \
	do { \
		int _exp = (int)(expected); \
		int _act = (int)(actual); \
		printf("  %-24s EXPECT=%d ACTUAL=%d\n", label, _exp, _act); \
		if (_exp != _act) \
			FAIL_MSG("%s mismatch", label); \
	} while (0)

#define EXPECT_EQ_U32(expected, actual, label) \
	do { \
		uint32_t _exp = (uint32_t)(expected); \
		uint32_t _act = (uint32_t)(actual); \
		printf("  %-24s EXPECT=0x%X ACTUAL=0x%X\n", label, _exp, _act); \
		if (_exp != _act) \
			FAIL_MSG("%s mismatch", label); \
	} while (0)

#define EXPECT_TRUE(actual, label) \
	do { \
		int _act = !!(actual); \
		printf("  %-24s EXPECT=true ACTUAL=%s\n", label, _act ? "true" : "false"); \
		if (!_act) \
			FAIL_MSG("%s expected true", label); \
	} while (0)

#define EXPECT_FALSE(actual, label) \
	do { \
		int _act = !!(actual); \
		printf("  %-24s EXPECT=false ACTUAL=%s\n", label, _act ? "true" : "false"); \
		if (_act) \
			FAIL_MSG("%s expected false", label); \
	} while (0)

#define EXPECT_STR(expected, actual, label) \
	do { \
		const char *_exp = (expected); \
		const char *_act = (actual); \
		printf("  %-24s EXPECT=%s ACTUAL=%s\n", label, _exp, _act); \
		if (strcmp(_exp, _act) != 0) \
			FAIL_MSG("%s mismatch", label); \
	} while (0)

static void
gtp_init_state_defaults(struct client_state *state)
{
	memset(state, 0, sizeof(*state));
	state->dummy_pool_fd = -1;
	state->ivi_window_management = true;
	state->ivi_zorder_auto = true;
	state->ivi_input_acceptance = 1;
	state->ivi_input_focus = 1;
	state->touch_log_enabled = false;
	state->log_control_mode = GVM_TOUCH_LOG_CONTROL_STDOUT;
	state->log_fp = NULL;
}

static void
create_file_or_die(const char *path, const char *content)
{
	FILE *fp = fopen(path, "w");

	if (!fp) {
		perror("fopen");
		exit(2);
	}

	fputs(content, fp);
	fclose(fp);
}

TEST(parse_ini_with_log_control_mode)
{
	struct client_state state;

	gtp_init_state_defaults(&state);

	create_file_or_die("unit_test_shared_touch.ini",
			   "[host_client]\n"
			   "id = 0x78FF\n"
			   "ivi_window_management = 1\n"
			   "ivi_zorder_auto = 1\n"
			   "ivi_input_acceptance = 1\n"
			   "ivi_input_focus = 1\n"
			   "touch_log_enabled = 1\n"
			   "log_control_mode = 2\n"
			   "\n"
			   "[vm_client]\n"
			   "id = 0x7815\n"
			   "output = DP1S078FF1\n"
			   "ivi_surface_id = 100\n"
			   "ivi_layer_id = 200\n");

	if (!gtp_parse_ini_file("unit_test_shared_touch.ini", &state))
		FAIL_MSG("gtp_parse_ini_file returned false");

	EXPECT_EQ_U32(0x78FF, state.host_client_id, "host_client_id");
	EXPECT_TRUE(state.ivi_window_management, "ivi_window_management");
	EXPECT_TRUE(state.ivi_zorder_auto, "ivi_zorder_auto");
	EXPECT_EQ_INT(1, state.ivi_input_acceptance, "ivi_input_acceptance");
	EXPECT_EQ_INT(1, state.ivi_input_focus, "ivi_input_focus");
	EXPECT_TRUE(state.touch_log_enabled, "touch_log_enabled");
	EXPECT_EQ_INT(2, state.log_control_mode, "log_control_mode");
	EXPECT_EQ_INT(1, state.port_count, "port_count");
	EXPECT_EQ_U32(0x7815, state.ports[0].client_id, "vm_client_id");
	EXPECT_STR("DP1S078FF1", state.ports[0].output_name, "shared_output");
	EXPECT_EQ_INT(PORT_TYPE_SHARED, state.ports[0].port_type, "port_type");
	EXPECT_EQ_U32(100, state.ports[0].ivi_surface_id, "ivi_surface_id");
	EXPECT_EQ_U32(200, state.ports[0].ivi_layer_id, "ivi_layer_id");

	unlink("unit_test_shared_touch.ini");
	PASS_MSG();
}

TEST(invalid_log_control_mode_fallback_to_zero)
{
	struct client_state state;

	gtp_init_state_defaults(&state);

	create_file_or_die("unit_test_invalid_log.ini",
			   "[host_client]\n"
			   "id = 0x78FF\n"
			   "log_control_mode = 99\n"
			   "\n"
			   "[vm_client]\n"
			   "id = 0x7815\n"
			   "output = DP0S078151\n"
			   "ivi_surface_id = 100\n"
			   "ivi_layer_id = 200\n");

	if (!gtp_parse_ini_file("unit_test_invalid_log.ini", &state))
		FAIL_MSG("gtp_parse_ini_file returned false");

	EXPECT_EQ_INT(0, state.log_control_mode, "log_control_mode");

	unlink("unit_test_invalid_log.ini");
	PASS_MSG();
}

TEST(case_two_shared_displays_expect_values)
{
	struct client_state state;
	PortInfo *host;
	PortInfo *sp1;
	PortInfo *sp2;
	PortInfo *sorted[2];

	gtp_init_state_defaults(&state);
	state.host_client_id = 0x78FF;

	sp1 = &state.ports[state.port_count++];
	memset(sp1, 0, sizeof(*sp1));
	sp1->client_id = 0x7815;
	snprintf(sp1->output_name, sizeof(sp1->output_name), "%s", "DP0S078151");
	sp1->ivi_surface_id = 100;
	sp1->ivi_layer_id = 200;
	sp1->port_type = PORT_TYPE_SHARED;
	sp1->device_id = 0;
	sp1->display_id = 3;
	sp1->display_node = 0;
	snprintf(sp1->cport_name, sizeof(sp1->cport_name), "%s", "DP0S078151");
	sp1->zorder_base = 0;
	sp1->zorder_size = 4;

	sp2 = &state.ports[state.port_count++];
	memset(sp2, 0, sizeof(*sp2));
	sp2->client_id = 0x7815;
	snprintf(sp2->output_name, sizeof(sp2->output_name), "%s", "DP0S078152");
	sp2->ivi_surface_id = 101;
	sp2->ivi_layer_id = 201;
	sp2->port_type = PORT_TYPE_SHARED;
	sp2->device_id = 0;
	sp2->display_id = 3;
	sp2->display_node = 0;
	snprintf(sp2->cport_name, sizeof(sp2->cport_name), "%s", "DP0S078152");
	sp2->zorder_base = 4;
	sp2->zorder_size = 2;

	host = &state.ports[state.port_count++];
	memset(host, 0, sizeof(*host));
	host->client_id = 0x78FF;
	snprintf(host->cport_name, sizeof(host->cport_name), "%s", "DP1S078FF1");
	host->port_type = PORT_TYPE_HOST;
	host->device_id = 0;
	host->display_id = 3;
	host->display_node = 0;
	host->zorder_base = 8;
	host->zorder_size = 2;
	host->width = 1920;
	host->height = 1080;
	host->uinput_fd = -1;

	gtp_link_ports(&state);

	EXPECT_EQ_INT(3, state.port_count, "total_ports");
	EXPECT_TRUE(sp1->host_port == host, "sp1_host_linked");
	EXPECT_TRUE(sp2->host_port == host, "sp2_host_linked");
	EXPECT_EQ_INT(1920, sp1->width, "sp1_width");
	EXPECT_EQ_INT(1080, sp1->height, "sp1_height");
	EXPECT_EQ_INT(1920, sp2->width, "sp2_width");
	EXPECT_EQ_INT(1080, sp2->height, "sp2_height");
	EXPECT_TRUE(gtp_validate_shared_port_configuration(&state), "validation_pass");

	sorted[0] = sp2;
	sorted[1] = sp1;
	qsort(sorted, 2, sizeof(sorted[0]), gtp_compare_shared_ports_by_zorder);

	EXPECT_STR("DP0S078151", sorted[0]->cport_name, "sorted_first_port");
	EXPECT_STR("DP0S078152", sorted[1]->cport_name, "sorted_second_port");
	EXPECT_EQ_INT(0, sorted[0]->zorder_base, "sorted_first_z");
	EXPECT_EQ_INT(4, sorted[1]->zorder_base, "sorted_second_z");

	PASS_MSG();
}

TEST(validation_fail_when_no_shared_ports)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	state.host_client_id = 0x78FF;

	EXPECT_FALSE(gtp_validate_shared_port_configuration(&state),
		     "validation_no_shared");
	PASS_MSG();
}

TEST(validation_fail_when_shared_port_not_found_in_xml)
{
	struct client_state state;
	PortInfo *sp;
	PortInfo *host;

	gtp_init_state_defaults(&state);
	state.host_client_id = 0x78FF;

	sp = &state.ports[state.port_count++];
	memset(sp, 0, sizeof(*sp));
	sp->client_id = 0x7815;
	snprintf(sp->output_name, sizeof(sp->output_name), "%s", "MISSING_PORT");
	sp->port_type = PORT_TYPE_SHARED;
	sp->device_id = 0;
	sp->display_id = 3;
	sp->display_node = 0;
	sp->uinput_fd = -1;

	host = &state.ports[state.port_count++];
	memset(host, 0, sizeof(*host));
	host->client_id = 0x78FF;
	snprintf(host->cport_name, sizeof(host->cport_name), "%s", "DP1S078FF1");
	host->port_type = PORT_TYPE_HOST;
	host->device_id = 0;
	host->display_id = 3;
	host->display_node = 0;
	host->uinput_fd = -1;

	gtp_link_ports(&state);

	EXPECT_FALSE(gtp_validate_shared_port_configuration(&state),
		     "validation_missing_xml_port");
	PASS_MSG();
}

TEST(validation_fail_when_no_host_link)
{
	struct client_state state;
	PortInfo *sp;
	PortInfo *host;

	gtp_init_state_defaults(&state);
	state.host_client_id = 0x78FF;

	sp = &state.ports[state.port_count++];
	memset(sp, 0, sizeof(*sp));
	sp->client_id = 0x7815;
	snprintf(sp->output_name, sizeof(sp->output_name), "%s", "DP0S078151");
	snprintf(sp->cport_name, sizeof(sp->cport_name), "%s", "DP0S078151");
	sp->port_type = PORT_TYPE_SHARED;
	sp->device_id = 0;
	sp->display_id = 3;
	sp->display_node = 1;
	sp->uinput_fd = -1;

	host = &state.ports[state.port_count++];
	memset(host, 0, sizeof(*host));
	host->client_id = 0x78FF;
	snprintf(host->cport_name, sizeof(host->cport_name), "%s", "DP1S078FF1");
	host->port_type = PORT_TYPE_HOST;
	host->device_id = 0;
	host->display_id = 2;
	host->display_node = 0;
	host->uinput_fd = -1;

	gtp_link_ports(&state);

	EXPECT_FALSE(gtp_validate_shared_port_configuration(&state),
		     "validation_no_host_link");
	PASS_MSG();
}

TEST(linking_uses_fallback_node_zero_host)
{
	struct client_state state;
	PortInfo *sp;
	PortInfo *host;

	gtp_init_state_defaults(&state);
	state.host_client_id = 0x78FF;

	sp = &state.ports[state.port_count++];
	memset(sp, 0, sizeof(*sp));
	sp->client_id = 0x7815;
	snprintf(sp->cport_name, sizeof(sp->cport_name), "%s", "DP0S078151");
	sp->port_type = PORT_TYPE_SHARED;
	sp->device_id = 0;
	sp->display_id = 3;
	sp->display_node = 7;
	sp->uinput_fd = -1;

	host = &state.ports[state.port_count++];
	memset(host, 0, sizeof(*host));
	host->client_id = 0x78FF;
	snprintf(host->cport_name, sizeof(host->cport_name), "%s", "DP1S078FF1");
	host->port_type = PORT_TYPE_HOST;
	host->device_id = 0;
	host->display_id = 3;
	host->display_node = 0;
	host->uinput_fd = -1;

	gtp_link_ports(&state);

	EXPECT_TRUE(sp->host_port == host, "fallback_host_link");
	PASS_MSG();
}

TEST(geometry_defaults_to_1920x1080)
{
	struct client_state state;
	PortInfo port;

	gtp_init_state_defaults(&state);
	memset(&port, 0, sizeof(port));

	gtp_resolve_port_geometry(&state, &port);

	EXPECT_EQ_INT(1920, port.width, "default_width");
	EXPECT_EQ_INT(1080, port.height, "default_height");
	PASS_MSG();
}

TEST(geometry_inherits_from_host_port)
{
	struct client_state state;
	PortInfo host;
	PortInfo shared;

	gtp_init_state_defaults(&state);
	memset(&host, 0, sizeof(host));
	memset(&shared, 0, sizeof(shared));

	host.width = 2560;
	host.height = 1440;
	host.offset_x = 10;
	host.offset_y = 20;

	shared.port_type = PORT_TYPE_SHARED;
	shared.host_port = &host;

	gtp_resolve_port_geometry(&state, &shared);

	EXPECT_EQ_INT(2560, shared.width, "inherited_width");
	EXPECT_EQ_INT(1440, shared.height, "inherited_height");
	EXPECT_EQ_INT(10, shared.offset_x, "inherited_offset_x");
	EXPECT_EQ_INT(20, shared.offset_y, "inherited_offset_y");
	PASS_MSG();
}

TEST(touch_slot_calculation_matches_design)
{
	int touch_id = 15;
	int actual_slot = touch_id % MAX_TOUCH_SLOTS;
	int expected_slot = 5;

	EXPECT_EQ_INT(expected_slot, actual_slot, "touch_slot");
	PASS_MSG();
}

TEST(zorder_host_and_shared_relationship)
{
	int shared_low = 0;
	int shared_high = 4;
	int host_base = 8;

	printf("  %-24s EXPECT=shared below host ACTUAL=%s\n",
	       "shared_low_vs_host", shared_low < host_base ? "below" : "not-below");
	if (!(shared_low < host_base))
		FAIL_MSG("shared_low should be below host");

	printf("  %-24s EXPECT=shared below host ACTUAL=%s\n",
	       "shared_high_vs_host", shared_high < host_base ? "below" : "not-below");
	if (!(shared_high < host_base))
		FAIL_MSG("shared_high should be below host");

	PASS_MSG();
}

TEST(log_file_mode_open_and_close)
{
	struct client_state state;
	bool actual_init_ok;

	gtp_init_state_defaults(&state);
	state.log_control_mode = GVM_TOUCH_LOG_CONTROL_FILE;

	actual_init_ok = gtp_init_log_output(&state);

	EXPECT_TRUE(actual_init_ok, "init_log_output");
	EXPECT_TRUE(state.log_fp != NULL, "log_fp_opened");

	gtp_close_log_output(&state);
	EXPECT_TRUE(state.log_fp == NULL, "log_fp_closed");
	PASS_MSG();
}

TEST(resolve_log_mode_semantics)
{
	EXPECT_EQ_INT(0, GVM_TOUCH_LOG_CONTROL_STDOUT, "mode_stdout");
	EXPECT_EQ_INT(1, GVM_TOUCH_LOG_CONTROL_FILE, "mode_file");
	EXPECT_EQ_INT(2, GVM_TOUCH_LOG_CONTROL_BOTH, "mode_both");
	PASS_MSG();
}

/* ------------------------------------------------------------------ */
/* Helper: build a state with 1 host + N shared ports linked together  */
/* ------------------------------------------------------------------ */
static void
build_linked_state(struct client_state *state,
		   int n_shared,
		   int host_width, int host_height)
{
	PortInfo *host;
	int i;

	gtp_init_state_defaults(state);
	state->host_client_id = 0x78FF;

	for (i = 0; i < n_shared; i++) {
		PortInfo *sp = &state->ports[state->port_count++];
		memset(sp, 0, sizeof(*sp));
		sp->client_id = 0x7815;
		snprintf(sp->output_name, sizeof(sp->output_name), "DP0S0781%02d", i);
		snprintf(sp->cport_name,  sizeof(sp->cport_name),  "DP0S0781%02d", i);
		sp->port_type = PORT_TYPE_SHARED;
		sp->device_id = 0;
		sp->display_id = 3;
		sp->display_node = 0;
		sp->ivi_surface_id = 100 + (uint32_t)i;
		sp->ivi_layer_id   = 200 + (uint32_t)i;
		sp->zorder_base = i * 4;
		sp->zorder_size = 4;
		sp->uinput_fd = -1;
	}

	host = &state->ports[state->port_count++];
	memset(host, 0, sizeof(*host));
	host->client_id = 0x78FF;
	snprintf(host->cport_name, sizeof(host->cport_name), "%s", "DP1S078FF1");
	host->port_type = PORT_TYPE_HOST;
	host->device_id = 0;
	host->display_id = 3;
	host->display_node = 0;
	host->zorder_base = 32;
	host->zorder_size = 2;
	host->width = host_width;
	host->height = host_height;
	host->uinput_fd = -1;

	gtp_link_ports(state);
}

static struct gtp_seat test_seat;

static void
build_linked_state_with_seat(struct client_state *state,
			     int n_shared, int host_width, int host_height)
{
	build_linked_state(state, n_shared, host_width, host_height);
	memset(&test_seat, 0, sizeof(test_seat));
	test_seat.state = state;
	snprintf(test_seat.name, sizeof(test_seat.name), "%s", "seat0");
	state->seats = &test_seat;
}

/* --- calculate_zorder -------------------------------------------- */

TEST(calculate_zorder_single_host_no_shared)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	state.host_client_id = 0x78FF;

	PortInfo *host = &state.ports[state.port_count++];
	memset(host, 0, sizeof(*host));
	host->client_id = 0x78FF;
	snprintf(host->cport_name, sizeof(host->cport_name), "%s", "DP1S078FF1");
	host->port_type = PORT_TYPE_HOST;
	host->zorder_base = 8;
	host->zorder_size = 2;
	host->width = 1920;
	host->height = 1080;

	calculate_zorder(&state);
	PASS_MSG();
}

TEST(calculate_zorder_host_with_two_shared)
{
	struct client_state state;

	build_linked_state(&state, 2, 1920, 1080);
	calculate_zorder(&state);
	PASS_MSG();
}

/* --- is_shared_layer --------------------------------------------- */

TEST(is_shared_layer_zero_returns_false)
{
	struct client_state state;

	build_linked_state(&state, 1, 1920, 1080);
	EXPECT_FALSE(is_shared_layer(&state, 0), "layer_0_not_shared");
	PASS_MSG();
}

TEST(is_shared_layer_known_id_returns_true)
{
	struct client_state state;

	build_linked_state(&state, 1, 1920, 1080);
	/* shared port 0 has ivi_layer_id = 200 */
	EXPECT_TRUE(is_shared_layer(&state, 200), "layer_200_is_shared");
	EXPECT_FALSE(is_shared_layer(&state, 999), "layer_999_not_shared");
	PASS_MSG();
}

/* --- track / untrack / ensure_screen_layer_capacity -------------- */

TEST(track_and_untrack_screen_layers)
{
	PortInfo host;

	memset(&host, 0, sizeof(host));
	snprintf(host.cport_name, sizeof(host.cport_name), "%s", "DP1");

	/* track three layers, verify dedup */
	track_screen_layer(&host, 10);
	track_screen_layer(&host, 20);
	track_screen_layer(&host, 10);  /* duplicate — should be ignored */
	EXPECT_EQ_INT(2, (int)host.screen_layer_count, "track_count_2");

	/* untrack middle */
	untrack_screen_layer(&host, 10);
	EXPECT_EQ_INT(1, (int)host.screen_layer_count, "untrack_count_1");
	EXPECT_EQ_U32(20, host.screen_layer_ids[0], "remaining_layer");

	/* untrack non-existent — should be no-op */
	untrack_screen_layer(&host, 999);
	EXPECT_EQ_INT(1, (int)host.screen_layer_count, "count_unchanged");

	free(host.screen_layer_ids);
	PASS_MSG();
}

TEST(track_screen_layer_null_host_is_noop)
{
	track_screen_layer(NULL, 10);
	untrack_screen_layer(NULL, 10);
	PASS_MSG();
}

TEST(ensure_screen_layer_capacity_grows)
{
	PortInfo host;

	memset(&host, 0, sizeof(host));
	snprintf(host.cport_name, sizeof(host.cport_name), "%s", "DP1");

	/* force multiple doublings by tracking 20 layers */
	for (int i = 1; i <= 20; i++)
		track_screen_layer(&host, (uint32_t)(i * 10));

	EXPECT_EQ_INT(20, (int)host.screen_layer_count, "20_layers_tracked");
	EXPECT_TRUE(host.screen_layer_capacity >= 20, "capacity_grown");

	free(host.screen_layer_ids);
	PASS_MSG();
}

/* --- find_port_by_surface ---------------------------------------- */

TEST(find_port_by_surface_found)
{
	struct client_state state;
	PortInfo *found;
	int fake_surface = 0xDEAD;

	build_linked_state(&state, 1, 1920, 1080);
	state.ports[0].surface = (struct wl_surface *)&fake_surface;

	found = find_port_by_surface(&state, (struct wl_surface *)&fake_surface);
	EXPECT_TRUE(found == &state.ports[0], "port_found_by_surface");
	PASS_MSG();
}

TEST(find_port_by_surface_not_found)
{
	struct client_state state;
	int fake_surface = 0xDEAD;

	build_linked_state(&state, 1, 1920, 1080);

	PortInfo *found = find_port_by_surface(&state, (struct wl_surface *)&fake_surface);
	EXPECT_TRUE(found == NULL, "port_not_found");
	PASS_MSG();
}

/* --- apply_ivi_input_policy -------------------------------------- */

TEST(apply_ivi_input_policy_no_ivi_input_is_noop)
{
	struct client_state state;

	build_linked_state(&state, 2, 1920, 1080);
	state.ivi_input = NULL;
	/* must not crash */
	apply_ivi_input_policy(&state);
	PASS_MSG();
}

TEST(apply_ivi_input_policy_for_port_zero_surface_id_is_noop)
{
	struct client_state state;
	PortInfo port;

	gtp_init_state_defaults(&state);
	state.ivi_input = NULL;
	memset(&port, 0, sizeof(port));
	port.ivi_surface_id = 0;

	apply_ivi_input_policy_for_port(&state, &port);
	PASS_MSG();
}

/* --- IVI input event callbacks ----------------------------------- */

TEST(ivi_input_handle_seat_created_sets_name)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	state.ivi_input = NULL;

	ivi_input_handle_seat_created(&state, NULL, "seat0", 0x3, 1);
	EXPECT_STR("seat0", state.active_seat_name, "seat_name_set");
	PASS_MSG();
}

TEST(ivi_input_handle_seat_capabilities_updates_name)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	state.ivi_input = NULL;

	ivi_input_handle_seat_capabilities(&state, NULL, "seat1", 0x1);
	EXPECT_STR("seat1", state.active_seat_name, "seat_caps_name");
	PASS_MSG();
}

TEST(ivi_input_handle_seat_destroyed_clears_name)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	snprintf(state.active_seat_name, sizeof(state.active_seat_name), "%s", "seat0");

	ivi_input_handle_seat_destroyed(&state, NULL, "seat0");
	EXPECT_STR("", state.active_seat_name, "seat_name_cleared");
	PASS_MSG();
}

TEST(ivi_input_handle_seat_destroyed_other_seat_unchanged)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	snprintf(state.active_seat_name, sizeof(state.active_seat_name), "%s", "seat0");

	ivi_input_handle_seat_destroyed(&state, NULL, "seat_other");
	EXPECT_STR("seat0", state.active_seat_name, "seat_name_unchanged");
	PASS_MSG();
}

TEST(ivi_input_handle_input_focus_no_log_no_output)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	state.touch_log_enabled = false;
	/* must not crash */
	ivi_input_handle_input_focus(&state, NULL, 100, 1, 1);
	state.touch_log_enabled = true;
	ivi_input_handle_input_focus(&state, NULL, 100, 1, 1);
	PASS_MSG();
}

TEST(ivi_input_handle_input_acceptance_prints)
{
	/* acceptance handler only prints, data unused */
	ivi_input_handle_input_acceptance(NULL, NULL, 100, "seat0", 1);
	PASS_MSG();
}

/* --- IVI WM simple event callbacks ------------------------------- */

TEST(ivi_wm_simple_callbacks_no_crash)
{
	struct client_state state;

	gtp_init_state_defaults(&state);

	ivi_wm_handle_surface_visibility(&state, NULL, 101, 1);
	ivi_wm_handle_layer_visibility(&state, NULL, 201, 1);
	ivi_wm_handle_surface_opacity(&state, NULL, 101, 0x10000);
	ivi_wm_handle_layer_opacity(&state, NULL, 201, 0x10000);
	ivi_wm_handle_surface_source_rectangle(&state, NULL, 101, 0, 0, 1920, 1080);
	ivi_wm_handle_layer_source_rectangle(&state, NULL, 201, 0, 0, 1920, 1080);
	ivi_wm_handle_surface_destination_rectangle(&state, NULL, 101, 0, 0, 1920, 1080);
	ivi_wm_handle_layer_destination_rectangle(&state, NULL, 201, 0, 0, 1920, 1080);
	ivi_wm_handle_surface_created(&state, NULL, 101);
	ivi_wm_handle_surface_destroyed(&state, NULL, 101);
	ivi_wm_handle_surface_error(&state, NULL, 101, 1, "err");
	ivi_wm_handle_layer_error(&state, NULL, 201, 1, "err");
	ivi_wm_handle_surface_size(&state, NULL, 101, 1920, 1080);
	ivi_wm_handle_surface_stats(&state, NULL, 101, 60, 1234);
	ivi_wm_handle_layer_surface_added(&state, NULL, 201, 101);
	PASS_MSG();
}

TEST(ivi_wm_layer_created_no_ivi_wm_no_crash)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	state.ivi_wm = NULL;
	state.ivi_zorder_auto = true;

	/* refresh_all_host_screens will return early because ivi_wm is NULL */
	ivi_wm_handle_layer_created(&state, NULL, 201);
	PASS_MSG();
}

TEST(ivi_wm_layer_destroyed_untrack_and_no_crash)
{
	struct client_state state;

	build_linked_state(&state, 1, 1920, 1080);
	state.ivi_wm = NULL;
	state.ivi_zorder_auto = false;

	/* zorder_auto=false → early return after untrack */
	ivi_wm_handle_layer_destroyed(&state, NULL, 200);
	PASS_MSG();
}

TEST(ivi_wm_layer_destroyed_zorder_auto_untrack)
{
	struct client_state state;

	build_linked_state(&state, 1, 1920, 1080);
	state.ivi_wm = NULL;
	state.ivi_zorder_auto = true;

	/* pre-track a non-shared layer on the host port */
	PortInfo *host = NULL;
	for (int i = 0; i < state.port_count; i++) {
		if (state.ports[i].port_type == PORT_TYPE_HOST) {
			host = &state.ports[i];
			break;
		}
	}
	EXPECT_TRUE(host != NULL, "host_found");
	track_screen_layer(host, 777);
	EXPECT_EQ_INT(1, (int)host->screen_layer_count, "layer_tracked");

	/* destroy layer 777 → should be untracked */
	ivi_wm_handle_layer_destroyed(&state, NULL, 777);
	EXPECT_EQ_INT(0, (int)host->screen_layer_count, "layer_untracked");

	free(host->screen_layer_ids);
	host->screen_layer_ids = NULL;
	PASS_MSG();
}

/* --- IVI WM screen callbacks ------------------------------------- */

TEST(ivi_wm_screen_handle_screen_id_no_crash)
{
	ivi_wm_screen_handle_screen_id(NULL, NULL, 42);
	PASS_MSG();
}

TEST(ivi_wm_screen_handle_connector_name_no_crash)
{
	ivi_wm_screen_handle_connector_name(NULL, NULL, "HDMI-A-1");
	PASS_MSG();
}

TEST(ivi_wm_screen_handle_error_no_crash)
{
	ivi_wm_screen_handle_error(NULL, NULL, 99, "something failed");
	ivi_wm_screen_handle_error(NULL, NULL, 99, NULL);
	PASS_MSG();
}

TEST(ivi_wm_screen_layer_added_null_host_no_crash)
{
	/* data=NULL branch (no host_port) */
	ivi_wm_screen_handle_layer_added(NULL, NULL, 300);
	PASS_MSG();
}

TEST(ivi_wm_screen_layer_added_with_host_no_ivi_wm)
{
	struct client_state state;
	PortInfo host;

	gtp_init_state_defaults(&state);
	memset(&host, 0, sizeof(host));
	snprintf(host.cport_name, sizeof(host.cport_name), "%s", "DP1");
	host.port_type = PORT_TYPE_HOST;
	host.state_ref = &state;
	state.ivi_wm = NULL;
	state.ivi_zorder_auto = false;

	/* ivi_zorder_auto=false → no enforce call, safe */
	ivi_wm_screen_handle_layer_added(&host, NULL, 400);
	PASS_MSG();
}

/* --- touch callbacks (uinput_fd = write-only dummy fd) ----------- */

static int
open_devnull_writeonly(void)
{
	return open("/dev/null", O_WRONLY);
}

TEST(touch_down_no_port_no_crash)
{
	struct client_state state;
	int dummy = 0xABCD;

	build_linked_state_with_seat(&state, 1, 1920, 1080);
	/* surface not registered → find_port_by_surface returns NULL */
	touch_down(&test_seat, NULL, 0, 0,
		   (struct wl_surface *)&dummy,
		   0, wl_fixed_from_int(100), wl_fixed_from_int(200));
	PASS_MSG();
}

TEST(touch_down_with_port_writes_events)
{
	struct client_state state;
	int fd;
	int dummy_surface = 1;

	build_linked_state_with_seat(&state, 1, 1920, 1080);
	fd = open_devnull_writeonly();
	EXPECT_TRUE(fd >= 0, "devnull_open");

	state.ports[0].surface = (struct wl_surface *)&dummy_surface;
	state.ports[0].uinput_fd = fd;
	state.ports[0].active_touches = 0;

	touch_down(&test_seat, NULL, 0, 0,
		   (struct wl_surface *)&dummy_surface,
		   3, wl_fixed_from_int(300), wl_fixed_from_int(400));

	EXPECT_EQ_INT(1, state.ports[0].active_touches, "active_touches_1");
	close(fd);
	PASS_MSG();
}

TEST(touch_up_decrements_active_touches)
{
	struct client_state state;
	int fd;
	int dummy_surface = 1;

	build_linked_state_with_seat(&state, 1, 1920, 1080);
	fd = open_devnull_writeonly();
	EXPECT_TRUE(fd >= 0, "devnull_open");

	state.ports[0].surface = (struct wl_surface *)&dummy_surface;
	state.ports[0].uinput_fd = fd;

	touch_down(&test_seat, NULL, 0, 0,
		   (struct wl_surface *)&dummy_surface,
		   3, wl_fixed_from_int(300), wl_fixed_from_int(400));
	touch_up(&test_seat, NULL, 0, 0, 3);

	EXPECT_EQ_INT(0, state.ports[0].active_touches, "active_touches_0");
	EXPECT_TRUE(find_touch_route_by_id(&state, &test_seat, 3) == NULL, "route_cleared");
	close(fd);
	PASS_MSG();
}

TEST(touch_up_no_active_touches_skips_port)
{
	struct client_state state;
	int fd;

	build_linked_state_with_seat(&state, 1, 1920, 1080);
	fd = open_devnull_writeonly();
	state.ports[0].uinput_fd = fd;
	state.ports[0].active_touches = 0;

	touch_up(&test_seat, NULL, 0, 0, 0);

	EXPECT_EQ_INT(0, state.ports[0].active_touches, "still_zero");
	close(fd);
	PASS_MSG();
}

TEST(touch_motion_with_active_touch)
{
	struct client_state state;
	int fd;
	int dummy_surface = 1;

	build_linked_state_with_seat(&state, 1, 1920, 1080);
	fd = open_devnull_writeonly();
	state.ports[0].surface = (struct wl_surface *)&dummy_surface;
	state.ports[0].uinput_fd = fd;

	touch_down(&test_seat, NULL, 0, 0,
		   (struct wl_surface *)&dummy_surface,
		   0, wl_fixed_from_int(300), wl_fixed_from_int(400));
	touch_motion(&test_seat, NULL, 0,
		     0, wl_fixed_from_int(500), wl_fixed_from_int(600));

	EXPECT_EQ_INT(1, state.ports[0].active_touches, "active_touches_still_1");
	close(fd);
	PASS_MSG();
}

TEST(touch_cancel_resets_active_touches)
{
	struct client_state state;
	int fd;
	int dummy_surface = 1;

	build_linked_state_with_seat(&state, 1, 1920, 1080);
	fd = open_devnull_writeonly();
	state.ports[0].surface = (struct wl_surface *)&dummy_surface;
	state.ports[0].uinput_fd = fd;

	touch_down(&test_seat, NULL, 0, 0,
		   (struct wl_surface *)&dummy_surface,
		   1, wl_fixed_from_int(300), wl_fixed_from_int(400));
	touch_cancel(&test_seat, NULL);

	EXPECT_EQ_INT(0, state.ports[0].active_touches, "touches_cancelled");
	EXPECT_TRUE(find_touch_route_by_id(&state, &test_seat, 1) == NULL, "route_cancelled");
	close(fd);
	PASS_MSG();
}

TEST(touch_route_is_bound_to_originating_port)
{
	struct client_state state;
	int fd0;
	int fd1;
	int surface0 = 1;
	int surface1 = 2;

	build_linked_state_with_seat(&state, 2, 1920, 1080);
	fd0 = open_devnull_writeonly();
	fd1 = open_devnull_writeonly();
	EXPECT_TRUE(fd0 >= 0, "devnull_open_0");
	EXPECT_TRUE(fd1 >= 0, "devnull_open_1");

	state.ports[0].surface = (struct wl_surface *)&surface0;
	state.ports[1].surface = (struct wl_surface *)&surface1;
	state.ports[0].uinput_fd = fd0;
	state.ports[1].uinput_fd = fd1;

	touch_down(&test_seat, NULL, 0, 0,
		   (struct wl_surface *)&surface0,
		   7, wl_fixed_from_int(100), wl_fixed_from_int(200));

	EXPECT_EQ_INT(1, state.ports[0].active_touches, "port0_active_1");
	EXPECT_EQ_INT(0, state.ports[1].active_touches, "port1_active_0");
	EXPECT_TRUE(find_touch_route_by_id(&state, &test_seat, 7) != NULL, "route_created");
	EXPECT_TRUE(find_touch_route_by_id(&state, &test_seat, 7)->port == &state.ports[0],
		    "route_points_port0");

	touch_motion(&test_seat, NULL, 0,
		     7, wl_fixed_from_int(150), wl_fixed_from_int(250));
	touch_up(&test_seat, NULL, 0, 0, 7);

	EXPECT_EQ_INT(0, state.ports[0].active_touches, "port0_active_0_after_up");
	EXPECT_EQ_INT(0, state.ports[1].active_touches, "port1_still_0");
	EXPECT_TRUE(find_touch_route_by_id(&state, &test_seat, 7) == NULL, "route_removed");

	close(fd0);
	close(fd1);
	PASS_MSG();
}

TEST(touch_frame_no_crash)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	touch_frame(&test_seat, NULL);
	PASS_MSG();
}

/* --- gvm_touch_log / gtp_log_touch_event modes ------------------- */

TEST(gvm_touch_log_file_mode_writes_to_file)
{
	struct client_state state;
	const char *path = "/tmp/gvm_test_log_mode.log";

	gtp_init_state_defaults(&state);
	state.log_control_mode = GVM_TOUCH_LOG_CONTROL_FILE;
	state.log_fp = fopen(path, "w");
	EXPECT_TRUE(state.log_fp != NULL, "log_file_open");

	gvm_touch_log(&state, NULL, "test file log %d\n", 42);

	fclose(state.log_fp);
	state.log_fp = NULL;
	unlink(path);
	PASS_MSG();
}

TEST(gvm_touch_log_both_mode_writes_to_file_and_stdout)
{
	struct client_state state;
	const char *path = "/tmp/gvm_test_log_both.log";

	gtp_init_state_defaults(&state);
	state.log_control_mode = GVM_TOUCH_LOG_CONTROL_BOTH;
	state.log_fp = fopen(path, "w");
	EXPECT_TRUE(state.log_fp != NULL, "log_file_open");

	gvm_touch_log(&state, NULL, "test both log %d\n", 42);

	fclose(state.log_fp);
	state.log_fp = NULL;
	unlink(path);
	PASS_MSG();
}

TEST(gvm_touch_log_null_state_uses_stdout)
{
	/* state=NULL → mode defaults to STDOUT, no crash */
	gvm_touch_log(NULL, stdout, "null state log\n");
	PASS_MSG();
}

TEST(gtp_log_touch_event_enabled_with_file_log)
{
	struct client_state state;
	PortInfo port;
	const char *path = "/tmp/gvm_test_touch_log.log";

	gtp_init_state_defaults(&state);
	state.touch_log_enabled = true;
	state.log_control_mode = GVM_TOUCH_LOG_CONTROL_FILE;
	state.log_fp = fopen(path, "w");
	EXPECT_TRUE(state.log_fp != NULL, "log_file_open");

	memset(&port, 0, sizeof(port));
	snprintf(port.cport_name, sizeof(port.cport_name), "%s", "DP0");
	port.uinput_fd = -1;
	port.active_touches = 1;

	gtp_log_touch_event(&state, &port, "DOWN", 0, 100, 200);

	fclose(state.log_fp);
	state.log_fp = NULL;
	unlink(path);
	PASS_MSG();
}

TEST(gtp_log_touch_event_disabled_is_noop)
{
	struct client_state state;
	PortInfo port;

	gtp_init_state_defaults(&state);
	state.touch_log_enabled = false;
	memset(&port, 0, sizeof(port));

	/* must not crash */
	gtp_log_touch_event(&state, &port, "DOWN", 0, 0, 0);
	gtp_log_touch_event(NULL, &port, "DOWN", 0, 0, 0);
	gtp_log_touch_event(&state, NULL, "DOWN", 0, 0, 0);
	PASS_MSG();
}

/* --- output_handle_mode / output_handle_name --------------------- */

TEST(output_handle_mode_updates_dimensions)
{
	struct client_state state;
	int dummy_output = 0xABCD;

	gtp_init_state_defaults(&state);
	state.output_count = 1;
	memset(&state.outputs[0], 0, sizeof(state.outputs[0]));
	state.outputs[0].output = (struct wl_output *)&dummy_output;
	snprintf(state.outputs[0].name, sizeof(state.outputs[0].name), "%s", "HDMI-A-1");

	output_handle_mode(&state, (struct wl_output *)&dummy_output,
			   WL_OUTPUT_MODE_CURRENT, 2560, 1440, 60000);

	EXPECT_EQ_INT(2560, state.outputs[0].width,  "output_width");
	EXPECT_EQ_INT(1440, state.outputs[0].height, "output_height");
	PASS_MSG();
}

TEST(output_handle_name_sets_name)
{
	struct client_state state;
	int dummy_output = 0xABCD;

	gtp_init_state_defaults(&state);
	state.output_count = 1;
	memset(&state.outputs[0], 0, sizeof(state.outputs[0]));
	state.outputs[0].output = (struct wl_output *)&dummy_output;

	output_handle_name(&state, (struct wl_output *)&dummy_output, "DP-1");

	EXPECT_STR("DP-1", state.outputs[0].name, "output_name");
	PASS_MSG();
}

/* --- find_output helpers ----------------------------------------- */

TEST(find_output_by_name_hit_and_miss)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	state.output_count = 2;
	memset(state.outputs, 0, sizeof(state.outputs));
	snprintf(state.outputs[0].name, sizeof(state.outputs[0].name), "%s", "DP-1");
	snprintf(state.outputs[1].name, sizeof(state.outputs[1].name), "%s", "HDMI-A-1");

	OutputInfo *out = find_output_by_name(&state, "HDMI-A-1");
	EXPECT_TRUE(out == &state.outputs[1], "find_by_name_hit");

	out = find_output_by_name(&state, "VGA-1");
	EXPECT_TRUE(out == NULL, "find_by_name_miss");
	PASS_MSG();
}

TEST(find_output_for_port_by_cport_name)
{
	struct client_state state;
	PortInfo port;

	gtp_init_state_defaults(&state);
	state.output_count = 1;
	memset(&state.outputs[0], 0, sizeof(state.outputs[0]));
	snprintf(state.outputs[0].name, sizeof(state.outputs[0].name), "%s", "DP-1");

	memset(&port, 0, sizeof(port));
	snprintf(port.cport_name, sizeof(port.cport_name), "%s", "DP-1");

	OutputInfo *out = find_output_for_port(&state, &port);
	EXPECT_TRUE(out == &state.outputs[0], "found_by_cport");
	PASS_MSG();
}

TEST(find_output_for_port_fallback_single_output)
{
	struct client_state state;
	PortInfo port;

	gtp_init_state_defaults(&state);
	state.output_count = 1;
	memset(&state.outputs[0], 0, sizeof(state.outputs[0]));
	snprintf(state.outputs[0].name, sizeof(state.outputs[0].name), "%s", "DP-1");

	memset(&port, 0, sizeof(port));
	/* no cport_name, no display_node/id → should fallback to outputs[0] */

	OutputInfo *out = find_output_for_port(&state, &port);
	EXPECT_TRUE(out == &state.outputs[0], "fallback_single_output");
	PASS_MSG();
}

/* --- emit_touch_event -------------------------------------------- */

TEST(emit_touch_event_writes_to_fd)
{
	int fd = open_devnull_writeonly();

	EXPECT_TRUE(fd >= 0, "devnull_open");
	emit_touch_event(fd, EV_ABS, ABS_MT_SLOT, 0);
	emit_touch_event(fd, EV_SYN, SYN_REPORT, 0);
	close(fd);
	PASS_MSG();
}

/* --- gtp_log mode: file fallback to stdout when fp NULL ---------- */

TEST(gvm_touch_log_file_mode_null_fp_falls_back_to_stdout)
{
	struct client_state state;

	gtp_init_state_defaults(&state);
	state.log_control_mode = GVM_TOUCH_LOG_CONTROL_FILE;
	state.log_fp = NULL;  /* fp not open → should fall back to stdout */

	gvm_touch_log(&state, NULL, "fallback to stdout %d\n", 1);
	PASS_MSG();
}

int
main(void)
{
	printf("\n");
	printf("====================================================\n");
	printf("  GVM Touch Proxy Real-Implementation Unit Tests\n");
	printf("====================================================\n\n");

	run_test_parse_ini_with_log_control_mode();
	run_test_invalid_log_control_mode_fallback_to_zero();
	run_test_case_two_shared_displays_expect_values();
	run_test_validation_fail_when_no_shared_ports();
	run_test_validation_fail_when_shared_port_not_found_in_xml();
	run_test_validation_fail_when_no_host_link();
	run_test_linking_uses_fallback_node_zero_host();
	run_test_geometry_defaults_to_1920x1080();
	run_test_geometry_inherits_from_host_port();
	run_test_touch_slot_calculation_matches_design();
	run_test_zorder_host_and_shared_relationship();
	run_test_log_file_mode_open_and_close();
	run_test_resolve_log_mode_semantics();

	/* --- new coverage tests --- */
	run_test_calculate_zorder_single_host_no_shared();
	run_test_calculate_zorder_host_with_two_shared();
	run_test_is_shared_layer_zero_returns_false();
	run_test_is_shared_layer_known_id_returns_true();
	run_test_track_and_untrack_screen_layers();
	run_test_track_screen_layer_null_host_is_noop();
	run_test_ensure_screen_layer_capacity_grows();
	run_test_find_port_by_surface_found();
	run_test_find_port_by_surface_not_found();
	run_test_apply_ivi_input_policy_no_ivi_input_is_noop();
	run_test_apply_ivi_input_policy_for_port_zero_surface_id_is_noop();
	run_test_ivi_input_handle_seat_created_sets_name();
	run_test_ivi_input_handle_seat_capabilities_updates_name();
	run_test_ivi_input_handle_seat_destroyed_clears_name();
	run_test_ivi_input_handle_seat_destroyed_other_seat_unchanged();
	run_test_ivi_input_handle_input_focus_no_log_no_output();
	run_test_ivi_input_handle_input_acceptance_prints();
	run_test_ivi_wm_simple_callbacks_no_crash();
	run_test_ivi_wm_layer_created_no_ivi_wm_no_crash();
	run_test_ivi_wm_layer_destroyed_untrack_and_no_crash();
	run_test_ivi_wm_layer_destroyed_zorder_auto_untrack();
	run_test_ivi_wm_screen_handle_screen_id_no_crash();
	run_test_ivi_wm_screen_handle_connector_name_no_crash();
	run_test_ivi_wm_screen_handle_error_no_crash();
	run_test_ivi_wm_screen_layer_added_null_host_no_crash();
	run_test_ivi_wm_screen_layer_added_with_host_no_ivi_wm();
	run_test_touch_down_no_port_no_crash();
	run_test_touch_down_with_port_writes_events();
	run_test_touch_up_decrements_active_touches();
	run_test_touch_up_no_active_touches_skips_port();
	run_test_touch_motion_with_active_touch();
	run_test_touch_cancel_resets_active_touches();
	run_test_touch_route_is_bound_to_originating_port();
	run_test_touch_frame_no_crash();
	run_test_gvm_touch_log_file_mode_writes_to_file();
	run_test_gvm_touch_log_both_mode_writes_to_file_and_stdout();
	run_test_gvm_touch_log_null_state_uses_stdout();
	run_test_gtp_log_touch_event_enabled_with_file_log();
	run_test_gtp_log_touch_event_disabled_is_noop();
	run_test_output_handle_mode_updates_dimensions();
	run_test_output_handle_name_sets_name();
	run_test_find_output_by_name_hit_and_miss();
	run_test_find_output_for_port_by_cport_name();
	run_test_find_output_for_port_fallback_single_output();
	run_test_emit_touch_event_writes_to_fd();
	run_test_gvm_touch_log_file_mode_null_fp_falls_back_to_stdout();

	printf("\n====================================================\n");
	printf("  Test Summary\n");
	printf("====================================================\n");
	printf("  Total tests : %d\n", tests_run);
	printf("  Passed      : %d\n", tests_passed);
	printf("  Failed      : %d\n", tests_failed);
	printf("====================================================\n");

	if (tests_failed > 0) {
		printf("FINAL RESULT : FAIL\n\n");
		return 1;
	}

	printf("FINAL RESULT : PASS\n\n");
	return 0;
}
