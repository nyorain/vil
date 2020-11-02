project('fuencaliente', ['c', 'cpp'],
	default_options: [
		'werror=true',
		'warning_level=3',
		'cpp_std=c++17',
		'c_std=c11',
	]
)

# needed shaders
glslang = find_program('glslangValidator')

overlay_shaders = [
	'overlay.frag',
	'overlay.vert',
]

spv = []
foreach s : ['overlay.frag', 'overlay.vert']
	varname = s.underscorify() + '_spv_data'
	spv += custom_target(
		s + '.spv.h', input : s, output : s + '.spv.h',
		command : [glslang, '-V', '--vn', varname, '-o', '@OUTPUT@', '@INPUT@'])
endforeach


cc = meson.get_compiler('cpp')
subdir('imgui')

dep_threads = dependency('threads', required: false)
dep_wayland = dependency('wayland-client')
dep_dlg = dependency('dlg', fallback: ['dlg', 'dlg_dep'])
dep_swa = dependency('swa',
	fallback: ['swa', 'swa_dep'],
	default_options: [
		'with-gl=false',
		'link-vulkan=false',
		'with-kms=false'
	])
dep_vkpp = dependency('vkpp MASTER',
	fallback: ['vkpp', 'vkpp_dep'],
	default_options: ['lib=false'])

src = files(
  'layer.cpp',
  'wayland.cpp',
  'overlay.cpp',
  'swa.cpp',
)

deps = [
	dep_threads,
	dep_dlg,
	dep_vkpp,
	dep_swa,
	dep_wayland,
	libimgui_core_dep,
]

vklayer_mesa_overlay = shared_library(
	'VkLayer_fuencaliente',
	[src, spv],
	gnu_symbol_visibility: 'hidden',
	dependencies: deps,
	include_directories: [],
	link_args: cc.get_supported_link_arguments(['-Wl,-Bsymbolic-functions', '-Wl,-z,relro']),
	install: true
)

install_data(
	files('VkLayer_fuencaliente.json'),
	install_dir : join_paths(get_option('datadir'), 'vulkan', 'explicit_layer.d'),
)
