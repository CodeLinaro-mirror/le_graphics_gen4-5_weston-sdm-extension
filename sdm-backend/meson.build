srcs_gbm_buffer_backend = [
	'gbm-buffer-backend.c',
	gbm_buffer_backend_protocol_c,
	gbm_buffer_backend_server_protocol_h,
]

deps_gbm_buffer_backend = [
	dep_libweston_private,
	dep_gbm,
]

lib_gbm_buffer_backend = shared_library(
	'gbm-buffer-backend',
	srcs_gbm_buffer_backend,
	include_directories: common_inc,
	dependencies: deps_gbm_buffer_backend,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston
)
dep_gbm_buffer_backend = declare_dependency(
	link_with: lib_gbm_buffer_backend,
	include_directories: include_directories('.')
)

srcs_screen_capture = [
	'screen-capture.c',
	screen_capture_protocol_c,
	screen_capture_server_protocol_h,
	gbm_buffer_backend_server_protocol_h,
]

deps_screen_capture = [
	dep_gbm_buffer_backend,
	dep_libweston_private,
	dep_gbm,
]

lib_screen_capture = shared_library(
	'screen-capture',
	srcs_screen_capture,
	include_directories: common_inc,
	dependencies: deps_screen_capture,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston,
	install_rpath: '$ORIGIN'
)
dep_screen_capture = declare_dependency(
	link_with: lib_screen_capture,
	include_directories: include_directories('.')
)
srcs_drm = [
        'compositor-sdm.c',
        'drm-service/drm_display.cpp',
        'drm-service/drm_display.h',
        'early-renderer.c',
	'screen-capture.c',
	linux_dmabuf_unstable_v1_protocol_c,
	linux_dmabuf_unstable_v1_server_protocol_h,
	gbm_buffer_backend_protocol_c,
	gbm_buffer_backend_server_protocol_h,
	screen_capture_protocol_c,
	screen_capture_server_protocol_h,
	presentation_time_server_protocol_h,
	pll_protocol_c,
	pll_server_protocol_h,
]

deps_drm = [
	dep_libdl,
	dep_libweston_private,
	dep_session_helper,
	dep_libdrm,
	dep_libinput,
	dependency('libudev', version: '>= 136'),
	dep_libinput_backend,
	dep_backlight,
	dep_libcutils,
	dep_libdrmutils,
	dep_libsdedrm,
	dep_gbm,
	dep_gbm_buffer_backend,
	dep_screen_capture,
]

if get_option('enable-early-boot')
	config_h.set('ENABLE_EARLY_BOOT', '1')
	deps_drm += dependency('threads')
endif

if get_option('enable-rt_schedule')
	config_h.set('ENABLE_RT_SCHEDULE', '1')
	deps_drm += cc.find_library('compresmgr_client')
endif

if get_option('enable-pageflip')
	config_h.set('ENABLE_PAGEFLIP', '1')
endif

plugin_drm = shared_library(
	'drm-backend',
	srcs_drm,
	include_directories: common_inc,
	dependencies: deps_drm,
	name_prefix: '',
	install: true,
	install_dir: dir_module_libweston,
	install_rpath: '$ORIGIN'
)

if get_option('enable-pm-dbus')
	deps_pm_ds_dbus = [
		dep_libsystemd,
		dep_libweston_private,
	]

	plugin_compositor_pm_ds_dbus = shared_library(
		'compositor-pm-ds-dbus',
		'compositor-pm-ds-dbus.c',
		include_directories: common_inc,
		dependencies: deps_pm_ds_dbus,
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston
	)
endif

if get_option('enable-pm-snservice')
	dep_glib = dependency('glib-2.0')

	deps_pm_ds_snservice = [
		dep_libpm_client,
		dep_libsystemd,
		dep_libweston_private,
		dep_glib,
	]

	plugin_compositor_pm_ds_snservice = shared_library(
		'compositor-pm-ds-snservice',
		'compositor-pm-ds-snservice.c',
		include_directories: common_inc,
		dependencies: deps_pm_ds_snservice,
		name_prefix: '',
		install: true,
		install_dir: dir_module_weston
	)
endif

