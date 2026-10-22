# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

project('gvm-touch-proxy',
	'c',
	version: '1.0.0',
	default_options: [
		'warning_level=3',
		'c_std=gnu99',
		'b_lundef=true',
	],
	meson_version: '>= 0.52.1',
	license: 'BSD-3-Clause-Clear',
)

cc = meson.get_compiler('c')

dep_wayland_client = dependency('wayland-client', version: '>= 1.18.0')
dep_libxml2 = dependency('libxml-2.0')
dep_threads = dependency('threads')

dep_libm = cc.find_library('m', required: false)
dep_libdl = cc.find_library('dl', required: false)

common_inc = include_directories('.')

wayland_scanner = find_program('wayland-scanner')

touch_only_xml = meson.current_source_dir() / '..' / 'protocol' / 'touch-only-surface.xml'
ivi_application_xml = meson.current_source_dir() / '..' / 'protocol' / 'ivi-application.xml'
xdg_shell_xml = dependency('wayland-protocols').get_variable(
	pkgconfig: 'pkgdatadir'
) / 'stable' / 'xdg-shell' / 'xdg-shell.xml'
viewporter_xml = dependency('wayland-protocols').get_variable(
	pkgconfig: 'pkgdatadir'
) / 'stable' / 'viewporter' / 'viewporter.xml'
ivi_input_xml = dependency('wayland-protocols').get_variable(
	pkgconfig: 'pkgdatadir'
) / 'ivi-input.xml'
ivi_wm_xml = dependency('wayland-protocols').get_variable(
	pkgconfig: 'pkgdatadir'
) / 'ivi-wm.xml'

touch_only_client_header = custom_target(
	'touch-only-surface-client-header',
	input: touch_only_xml,
	output: 'touch-only-surface-client-protocol.h',
	command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

touch_only_protocol_c = custom_target(
	'touch-only-surface-protocol-c',
	input: touch_only_xml,
	output: 'touch-only-surface-protocol.c',
	command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_header = custom_target(
	'xdg-shell-client-header',
	input: xdg_shell_xml,
	output: 'xdg-shell-client-protocol.h',
	command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_protocol_c = custom_target(
	'xdg-shell-protocol-c',
	input: xdg_shell_xml,
	output: 'xdg-shell-protocol.c',
	command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

viewporter_client_header = custom_target(
	'viewporter-client-header',
	input: viewporter_xml,
	output: 'viewporter-client-protocol.h',
	command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

viewporter_protocol_c = custom_target(
	'viewporter-protocol-c',
	input: viewporter_xml,
	output: 'viewporter-protocol.c',
	command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ivi_application_client_header = custom_target(
	'ivi-application-client-header',
	input: ivi_application_xml,
	output: 'ivi-application-client-protocol.h',
	command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

ivi_application_protocol_c = custom_target(
	'ivi-application-protocol-c',
	input: ivi_application_xml,
	output: 'ivi-application-protocol.c',
	command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ivi_input_client_header = custom_target(
	'ivi-input-client-header',
	input: ivi_input_xml,
	output: 'ivi-input-client-protocol.h',
	command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

ivi_input_protocol_c = custom_target(
	'ivi-input-protocol-c',
	input: ivi_input_xml,
	output: 'ivi-input-protocol.c',
	command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

ivi_wm_client_header = custom_target(
	'ivi-wm-client-header',
	input: ivi_wm_xml,
	output: 'ivi-wm-client-protocol.h',
	command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

ivi_wm_protocol_c = custom_target(
	'ivi-wm-protocol-c',
	input: ivi_wm_xml,
	output: 'ivi-wm-protocol.c',
	command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

protocol_headers = [
	touch_only_client_header,
	xdg_shell_client_header,
	viewporter_client_header,
	ivi_application_client_header,
	ivi_input_client_header,
	ivi_wm_client_header,
]

protocol_sources = [
	touch_only_protocol_c,
	xdg_shell_protocol_c,
	viewporter_protocol_c,
	ivi_application_protocol_c,
	ivi_input_protocol_c,
	ivi_wm_protocol_c,
]

gvm_touch_proxy_deps = [
	dep_wayland_client,
	dep_libxml2,
	dep_threads,
]

if dep_libm.found()
	gvm_touch_proxy_deps += dep_libm
endif

if dep_libdl.found()
	gvm_touch_proxy_deps += dep_libdl
endif

gvm_touch_proxy_inc = include_directories('.')

gvm_touch_proxy = executable(
	'gvm-touch-proxy',
	[
		'gvm-touch-proxy.c',
		protocol_sources,
		protocol_headers,
	],
	include_directories: gvm_touch_proxy_inc,
	dependencies: gvm_touch_proxy_deps,
	install: true,
	install_dir: get_option('bindir'),
)

gvm_touch_monitor = executable(
	'pvm-touch-monitor',
	[
		'test/pvm-touch-monitor.c',
	],
	include_directories: gvm_touch_proxy_inc,
	dependencies: [],
	install: true,
	install_dir: get_option('bindir'),
)

install_data(
	'shared_touch.ini',
	install_dir: get_option('bindir'),
)

gvm_touch_proxy_test = executable(
	'gvm-touch-proxy-test',
	[
		'test/gvm-touch-proxy-test.c',
		'gvm-touch-proxy.c',
		protocol_sources,
		protocol_headers,
	],
	include_directories: gvm_touch_proxy_inc,
	dependencies: gvm_touch_proxy_deps,
	c_args: ['-DGVM_TOUCH_PROXY_UNIT_TEST'],
	install: get_option('b_coverage'),
	install_dir: get_option('bindir'),
)

test('gvm-touch-proxy unit tests', gvm_touch_proxy_test)

if get_option('b_coverage')
	message('Coverage enabled: gvm-touch-proxy-test will be installed for target-side execution')
endif
