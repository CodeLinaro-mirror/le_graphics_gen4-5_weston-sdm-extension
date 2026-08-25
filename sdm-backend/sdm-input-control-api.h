/*
 *    Cross-plugin API to let the PM-notify plugin suspend/resume the sdm
 *    backend's input source. Backend registers via weston_plugin_api_register(),
 *    PM plugin fetches via weston_plugin_api_get().
 *
 *    Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *    SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef SDM_INPUT_CONTROL_API_H
#define SDM_INPUT_CONTROL_API_H

#ifdef __cplusplus
extern "C" {
#endif

struct weston_compositor;

#define SDM_INPUT_CONTROL_API_NAME "sdm_input_control_v1"

struct sdm_input_control_api {
	/* Suspend the backend input source (idempotent). */
	void (*suspend_input)(struct weston_compositor *compositor);

	/* Resume the backend input source (idempotent). */
	void (*resume_input)(struct weston_compositor *compositor);
};

#ifdef __cplusplus
}
#endif

#endif /* SDM_INPUT_CONTROL_API_H */
