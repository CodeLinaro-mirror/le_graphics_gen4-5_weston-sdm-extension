/*
 * pvm-touch-monitor.c
 *
 * Test tool that creates a virtual multi-touch device via /dev/uinput and
 * continuously sends randomly-generated touch events (down - move - up) at
 * random screen coordinates, useful for stress-testing the touch pipeline
 * without a physical touch panel.
 *
 * Intended usage in this project:
 *   - This tool runs on the host.
 *   - It creates a touch device on the seat that Weston is monitoring (typically "seat0").
 *   - Weston will see this as a normal touch device and deliver wl_touch events
 *     to clients such as gvm-touch-proxy.
 *
 * NOTE:
 *   - This is a minimal test/monitor tool. For production usage you may want to:
 *       * read real input from another device or source,
 *       * support multiple fingers / configurable paths,
 *       * integrate with logging / configuration frameworks.
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TOUCH_MONITOR_DEVICE_NAME "host touch monitor"
#define HOST_TOUCH_LOG_FILE "/tmp/host_touch.log"
#define TOUCH_MAX_X 1920
#define TOUCH_MAX_Y 1080
#define TOUCH_MONITOR_VENDOR_ID 0x1234
#define TOUCH_MONITOR_PRODUCT_BASE 0x0001

/* Simple logging helpers */
static FILE *host_touch_log_fp;

static void
log_err(const char *msg)
{
	fprintf(stderr, "[pvm-touch-monitor][ERR] %s: %s\n", msg, strerror(errno));
	if (host_touch_log_fp) {
		fprintf(host_touch_log_fp, "[pvm-touch-monitor][ERR] %s: %s\n",
			msg, strerror(errno));
		fflush(host_touch_log_fp);
	}
}

static void
log_info(const char *msg)
{
	if (host_touch_log_fp) {
		fprintf(host_touch_log_fp, "[pvm-touch-monitor][INFO] %s\n", msg);
		fflush(host_touch_log_fp);
	}
}

static void
log_touch_point(int device_id, const char *phase, int32_t x, int32_t y)
{
	if (host_touch_log_fp) {
		fprintf(host_touch_log_fp,
			"[pvm-touch-monitor][TOUCH][device=%d] %s x=%d y=%d\n",
			device_id, phase, x, y);
		fflush(host_touch_log_fp);
	}
}

/*
 * Write a single input_event to the uinput fd.
 */
static int
emit_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));

	clock_gettime(CLOCK_MONOTONIC, &ev.time);
	ev.type = type;
	ev.code = code;
	ev.value = value;

	if (write(fd, &ev, sizeof(ev)) < 0) {
		log_err("write input_event failed");
		return -1;
	}

	return 0;
}

/*
 * Emit a SYN_REPORT to mark the end of a frame.
 */
static int
emit_syn_report(int fd)
{
	return emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

/*
 * Create a basic multi-touch device via /dev/uinput.
 *
 * The abs ranges are arbitrary defaults; adjust as needed to match
 * your compositor / output resolution.
 */
static int
create_touch_device(int id)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	char device_name[UINPUT_MAX_NAME_SIZE];
	char phys_name[64];
	char log_msg[160];

	if (fd < 0) {
		log_err("open /dev/uinput failed");
		return -1;
	}

	if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0) {
		log_err("UI_SET_EVBIT failed");
		goto error;
	}

	if (ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
	    ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER) < 0) {
		log_err("UI_SET_KEYBIT failed");
		goto error;
	}

	if (ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) {
		log_err("UI_SET_PROPBIT INPUT_PROP_DIRECT failed");
		goto error;
	}

	if (ioctl(fd, UI_SET_ABSBIT, ABS_X) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_Y) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_PRESSURE) < 0 ||
	    ioctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE) < 0) {
		log_err("UI_SET_ABSBIT failed");
		goto error;
	}

	struct uinput_setup usetup;
	memset(&usetup, 0, sizeof(usetup));

	usetup.id.bustype = BUS_VIRTUAL;
	usetup.id.vendor  = TOUCH_MONITOR_VENDOR_ID;
	usetup.id.product = TOUCH_MONITOR_PRODUCT_BASE + id;
	usetup.id.version = 1;
	snprintf(device_name, sizeof(device_name), "%s%d",
		 TOUCH_MONITOR_DEVICE_NAME, id);
	snprintf(phys_name, sizeof(phys_name),
		 "pvm-touch-monitor/input%d", id);
	snprintf(usetup.name, sizeof(usetup.name), "%s", device_name);

#ifdef UI_SET_PHYS
	if (ioctl(fd, UI_SET_PHYS, phys_name) < 0)
		log_err("UI_SET_PHYS failed");
#endif

	if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
		log_err("UI_DEV_SETUP failed");
		goto error;
	}

	struct uinput_abs_setup abs_setup;
	memset(&abs_setup, 0, sizeof(abs_setup));

	abs_setup.code = ABS_X;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = TOUCH_MAX_X;
	abs_setup.absinfo.fuzz    = 0;
	abs_setup.absinfo.flat    = 0;
	abs_setup.absinfo.resolution = 0;

	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
		log_err("UI_ABS_SETUP ABS_X failed");
		goto error;
	}

	abs_setup.code = ABS_Y;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = TOUCH_MAX_Y;

	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
		log_err("UI_ABS_SETUP ABS_Y failed");
		goto error;
	}

	abs_setup.code = ABS_PRESSURE;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = 255;

	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
		log_err("UI_ABS_SETUP ABS_PRESSURE failed");
		goto error;
	}

	abs_setup.code = ABS_MT_PRESSURE;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = 255;

	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
		log_err("UI_ABS_SETUP ABS_MT_PRESSURE failed");
		goto error;
	}

	abs_setup.code = ABS_MT_POSITION_X;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = TOUCH_MAX_X;

	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
		log_err("UI_ABS_SETUP ABS_MT_POSITION_X failed");
		goto error;
	}

	abs_setup.code = ABS_MT_POSITION_Y;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = TOUCH_MAX_Y;

	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
		log_err("UI_ABS_SETUP ABS_MT_POSITION_Y failed");
		goto error;
	}

	/* One slot is enough for a basic 1-finger test */
	abs_setup.code = ABS_MT_SLOT;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = 9;

	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
		log_err("UI_ABS_SETUP ABS_MT_SLOT failed");
		goto error;
	}

	abs_setup.code = ABS_MT_TRACKING_ID;
	abs_setup.absinfo.minimum = 0;
	abs_setup.absinfo.maximum = 65535;

	if (ioctl(fd, UI_ABS_SETUP, &abs_setup) < 0) {
		log_err("UI_ABS_SETUP ABS_MT_TRACKING_ID failed");
		goto error;
	}

	if (ioctl(fd, UI_DEV_CREATE) < 0) {
		log_err("UI_DEV_CREATE failed");
		goto error;
	}

	snprintf(log_msg, sizeof(log_msg),
		 "Created virtual touch device via /dev/uinput: name=%s vendor=0x%04x product=0x%04x phys=%s",
		 device_name, TOUCH_MONITOR_VENDOR_ID,
		 TOUCH_MONITOR_PRODUCT_BASE + id, phys_name);
	log_info(log_msg);
	return fd;

error:
	close(fd);
	return -1;
}

#define TOUCH_MOVE_INTERVAL_MS 10
#define TOUCH_MOVE_DURATION_MS 1000

static int
sleep_ms(long milliseconds)
{
	struct timespec req;
	struct timespec rem;

	if (milliseconds < 0) {
		errno = EINVAL;
		return -1;
	}

	req.tv_sec = milliseconds / 1000;
	req.tv_nsec = (milliseconds % 1000) * 1000000L;

	while (nanosleep(&req, &rem) < 0) {
		if (errno != EINTR) {
			log_err("nanosleep failed");
			return -1;
		}

		req = rem;
	}

	return 0;
}

/*
 * Send a single 1-finger touch sequence:
 *   - slot 0 tracking_id = 1
 *   - down at (x1, y1)
 *   - move towards (x2, y2) every 10 ms
 *   - up
 */
static int
send_touch_sequence(int fd, int device_id, int32_t x1, int32_t y1, int32_t x2,
		    int32_t y2)
{
	int ret = 0;
	int steps = TOUCH_MOVE_DURATION_MS / TOUCH_MOVE_INTERVAL_MS;
	int step;

	if (steps <= 0)
		steps = 1;

	log_info("Sending touch down sequence");
	log_touch_point(device_id, "DOWN", x1, y1);
	ret |= emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
	ret |= emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
	ret |= emit_event(fd, EV_ABS, ABS_MT_POSITION_X, x1);
	ret |= emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y1);
	ret |= emit_event(fd, EV_ABS, ABS_X, x1);
	ret |= emit_event(fd, EV_ABS, ABS_Y, y1);
	ret |= emit_event(fd, EV_ABS, ABS_PRESSURE, 50);
	ret |= emit_event(fd, EV_ABS, ABS_MT_PRESSURE, 50);
	ret |= emit_event(fd, EV_KEY, BTN_TOUCH, 1);
	ret |= emit_event(fd, EV_KEY, BTN_TOOL_FINGER, 1);
	ret |= emit_syn_report(fd);

	if (ret < 0)
		return -1;

	log_info("Sending touch move sequence every 10 ms");
	for (step = 1; step <= steps; step++) {
		int32_t x = x1 + (x2 - x1) * step / steps;
		int32_t y = y1 + (y2 - y1) * step / steps;

		ret = 0;
		log_touch_point(device_id, "MOVE", x, y);
		ret |= emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
		ret |= emit_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
		ret |= emit_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
		ret |= emit_event(fd, EV_ABS, ABS_X, x);
		ret |= emit_event(fd, EV_ABS, ABS_Y, y);
		ret |= emit_event(fd, EV_ABS, ABS_PRESSURE, 50);
		ret |= emit_event(fd, EV_ABS, ABS_MT_PRESSURE, 50);
		ret |= emit_syn_report(fd);

		if (ret < 0)
			return -1;

		if (step < steps && sleep_ms(TOUCH_MOVE_INTERVAL_MS) < 0)
			return -1;
	}

	log_info("Sending touch up sequence");
	log_touch_point(device_id, "UP", x2, y2);
	ret = 0;
	ret |= emit_event(fd, EV_ABS, ABS_MT_SLOT, 0);
	ret |= emit_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
	ret |= emit_event(fd, EV_ABS, ABS_PRESSURE, 0);
	ret |= emit_event(fd, EV_ABS, ABS_MT_PRESSURE, 0);
	ret |= emit_event(fd, EV_KEY, BTN_TOUCH, 0);
	ret |= emit_event(fd, EV_KEY, BTN_TOOL_FINGER, 0);
	ret |= emit_syn_report(fd);

	if (ret < 0)
		return -1;

	return 0;
}

int
main(int argc, char *argv[])
{
	int *fds = NULL;
	int device_count = 1;
	int32_t x1;
	int32_t y1;
	int32_t x2;
	int32_t y2;
	int i;
	char log_msg[128];

	host_touch_log_fp = fopen(HOST_TOUCH_LOG_FILE, "a");
	if (!host_touch_log_fp)
		fprintf(stderr, "[pvm-touch-monitor][ERR] open %s failed: %s\n",
			HOST_TOUCH_LOG_FILE, strerror(errno));
	else
		setvbuf(host_touch_log_fp, NULL, _IOLBF, 0);

	if (argc > 1) {
		device_count = atoi(argv[1]);
		if (device_count <= 0) {
			fprintf(stderr, "[pvm-touch-monitor][ERR] invalid device count: %s\n",
				argv[1]);
			if (host_touch_log_fp)
				fclose(host_touch_log_fp);
			return EXIT_FAILURE;
		}
	}

	srand((unsigned int)time(NULL));

	fds = calloc(device_count, sizeof(int));
	if (!fds) {
		log_err("calloc for device fds failed");
		if (host_touch_log_fp)
			fclose(host_touch_log_fp);
		return EXIT_FAILURE;
	}

	for (i = 0; i < device_count; i++) {
		fds[i] = create_touch_device(i);
		if (fds[i] < 0) {
			log_err("failed to create virtual touch device");
			goto error;
		}

		snprintf(log_msg, sizeof(log_msg),
			 "Created virtual touch device id=%d", i);
		log_info(log_msg);
	}

	snprintf(log_msg, sizeof(log_msg),
		 "Start continuous touch sequences for %d device(s). Press Ctrl+C to exit.",
		 device_count);
	log_info(log_msg);

	for (;;) {
		for (i = 0; i < device_count; i++) {
			x1 = rand() % (TOUCH_MAX_X + 1);
			y1 = rand() % (TOUCH_MAX_Y + 1);
			x2 = rand() % (TOUCH_MAX_X + 1);
			y2 = rand() % (TOUCH_MAX_Y + 1);

			if (send_touch_sequence(fds[i], i, x1, y1, x2, y2) < 0) {
				snprintf(log_msg, sizeof(log_msg),
					 "failed to send touch sequence for device id=%d", i);
				log_info(log_msg);
				goto error;
			}
		}

		if (sleep_ms(200) < 0)
			goto error;
	}

error:
	if (fds) {
		for (i = 0; i < device_count; i++) {
			if (fds[i] >= 0) {
				ioctl(fds[i], UI_DEV_DESTROY);
				close(fds[i]);
			}
		}
		free(fds);
	}

	if (host_touch_log_fp)
		fclose(host_touch_log_fp);
	return EXIT_FAILURE;
}
