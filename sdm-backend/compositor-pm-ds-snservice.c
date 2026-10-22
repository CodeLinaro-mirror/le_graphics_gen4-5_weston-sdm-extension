/*
*    This file provide a module to deal with deep sleep.
*    Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
*    SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include <stdlib.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <pm_client_lib.h>
#include <pcm_client_lib.h>
#include <wayland-server.h>
#include <libweston/zalloc.h>
#include <libweston/libweston.h>
#include <libweston/plugin-registry.h>
#include "shared/helpers.h"
#include "sdm-input-control-api.h"

#ifndef EOK
#define EOK 0
#endif

#ifndef E_NOK
#define E_NOK -1
#endif

#define PM_DISPLAY_CLIENT_NAME "pm-client-weston"

/*
 * Fetch the input-control vtable published by the sdm backend via the plugin
 * registry. Returns NULL if the backend did not register it.
 */
static const struct sdm_input_control_api *
get_input_control_api(struct weston_compositor *compositor)
{
	return weston_plugin_api_get(compositor, SDM_INPUT_CONTROL_API_NAME,
				     sizeof(struct sdm_input_control_api));
}

enum {
	EVENT_SUSPEND = 1,
	EVENT_RESUME  = 2,
	EVENT_MAX,
};

struct snservice_notifier {
	struct wl_event_source *snservice_source;
	struct wl_listener compositor_destroy_listener;
	struct weston_compositor *compositor;
	int pm_ev_fd;
	struct pm_ops_s ops;
	pm_client_t pm_handle;
};

struct snservice_notifier snservice_notifier;

static int
weston_compositor_ds_suspend(void *data, enum PM_MODE mode)
{
	struct snservice_notifier *notifier = (struct snservice_notifier *)data;
	uint64_t u = EVENT_SUSPEND;

	if (!notifier) {
		weston_log("SN Suspend: notifier null \n");
		return E_NOK;
	}

	weston_log("SN Suspend: Trigger suspend event \n");
	write(notifier->pm_ev_fd, &u, sizeof(uint64_t));

	return EOK;
}

static int
weston_compositor_ds_resume(void *data, enum PM_MODE mode)
{
	struct snservice_notifier *notifier = (struct snservice_notifier *)data;
	uint64_t u = EVENT_RESUME;

	if (!notifier) {
		weston_log("SN Resume: notifier null \n");
		return E_NOK;
	}

	weston_log("SN Resume: Trigger resume event \n");
	write(notifier->pm_ev_fd, &u, sizeof(uint64_t));

	return EOK;
}

static int
weston_compositor_lpm_impose(void *data, int level)
{
	struct snservice_notifier *notifier = (struct snservice_notifier *)data;
	uint64_t u = EVENT_MAX;
	int32_t ret = EOK;

	if (!notifier) {
		weston_log("LPM Impose: notifier null \n");
		return E_NOK;
	}

	switch (level)
	{
		case VPP_VLVL_OFF:
			u = EVENT_SUSPEND;
			break;
		case VPP_VLVL_SVS:
		case VPP_VLVL_SVS_L1:
		case VPP_VLVL_NOM:
		case VPP_VLVL_TUR:
		case VPP_VLVL_TUR_L1:
			u = EVENT_RESUME;
			break;
		default:
			weston_log("LPM Impose: Unknown impose level %d\n", level);
			ret = E_NOK;
			break;
	}
	weston_log("LPM Impose: Trigger impose event with level %d\n", level);
	write(notifier->pm_ev_fd, &u, sizeof(uint64_t));

	return ret;
}


static void
snservice_ds_handler(int fd, uint32_t mask, void* data)
{
	struct snservice_notifier *notifier = data;
	const struct sdm_input_control_api *input_ctl =
		get_input_control_api(notifier->compositor);
	uint64_t event_type;

	read(fd, &event_type, sizeof(uint64_t));
	switch (event_type)
	{
		case EVENT_SUSPEND:
			if (input_ctl)
				input_ctl->suspend_input(notifier->compositor);
			weston_compositor_sleep(notifier->compositor);
			weston_log("PM Suspend: weston sleep \n");
			break;
		case EVENT_RESUME:
			if (input_ctl)
				input_ctl->resume_input(notifier->compositor);
			weston_compositor_wake(notifier->compositor);
			weston_log("PM Resume: weston wakeup \n");
			break;
		default:
			weston_log("PM: Unknown pm event type %d\n", event_type);
			break;
	}
}

static int
weston_compositor_enable_ds_event(struct snservice_notifier *notifier)
{
	struct wl_event_loop *loop;

	notifier->pm_ev_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (notifier->pm_ev_fd < 0)
		return E_NOK;

	loop = wl_display_get_event_loop(notifier->compositor->wl_display);
	if (!loop) {
		weston_log("SN: no loop get \n");
		goto err_get_loop;
	}

	notifier->snservice_source =
			wl_event_loop_add_fd(loop, notifier->pm_ev_fd,
					WL_EVENT_READABLE, snservice_ds_handler, notifier);

	return EOK;

err_get_loop:
	close(notifier->pm_ev_fd);
	notifier->pm_ev_fd = -1;

	return E_NOK;
}

static void
weston_compositor_disable_ds_event(struct snservice_notifier *notifier)
{
	if (notifier->snservice_source) {
		wl_event_source_remove(notifier->snservice_source);
		notifier->snservice_source = NULL;
	}

	if (notifier->pm_ev_fd) {
		close(notifier->pm_ev_fd);
		notifier->pm_ev_fd = -1;
	}
}

static void
weston_compositor_destroy_snservice_listener(struct wl_listener *listener, void *data)
{
	struct snservice_notifier *notifier;

	notifier = container_of(listener, struct snservice_notifier, compositor_destroy_listener);

	if (notifier->pm_handle)
		pm_deregister(notifier->pm_handle);

	weston_compositor_disable_ds_event(notifier);
	wl_list_remove(&notifier->compositor_destroy_listener.link);
}

WL_EXPORT int
wet_module_init(struct weston_compositor *compositor, int *argc, char *argv[])
{
	int32_t ret = EOK;

	if (!compositor)
		return E_NOK;

	if (!weston_compositor_add_destroy_listener_once(compositor,
			&snservice_notifier.compositor_destroy_listener,
			weston_compositor_destroy_snservice_listener)) {
		weston_log("SN: weston_compositor_add_destroy_listener_once fail \n");
		goto err_out;
	}

	snservice_notifier.compositor = compositor;
	snservice_notifier.ops.pm_enter = weston_compositor_ds_suspend;
	snservice_notifier.ops.pm_exit = weston_compositor_ds_resume;
	snservice_notifier.ops.impose = weston_compositor_lpm_impose;

	ret = weston_compositor_enable_ds_event(&snservice_notifier);
	if (ret) {
		weston_log("SN: weston_compositor_enable_ds_event fail \n");
		goto err_event;
	}

	ret = pm_register((char *)PM_DISPLAY_CLIENT_NAME,
			&snservice_notifier.ops,
			(void*)&snservice_notifier,
			&snservice_notifier.pm_handle);
	if (ret) {
		weston_log("SN: pm_register fail, ret %d\n", ret);
		goto err_register;
	}

	weston_log("SN: register snserivce ops for client %s\n", PM_DISPLAY_CLIENT_NAME);

	return ret;

err_register:
	weston_compositor_disable_ds_event(&snservice_notifier);
err_event:
	wl_list_remove(&snservice_notifier.compositor_destroy_listener.link);
err_out:
	return E_NOK;
}
