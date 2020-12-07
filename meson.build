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
foreach s : overlay_shaders
	varname = s.underscorify() + '_spv_data'
	spv += custom_target(
		s + '.spv.h',
		input: files('src/data/' + s),
		output: s + '.spv.h',
		command : [glslang, '-V', '--vn', varname, '-o', '@OUTPUT@', '@INPUT@'])
endforeach


cc = meson.get_compiler('cpp')
subdir('src/imgui')

dep_threads = dependency('threads', required: false)
dep_dlg = dependency('dlg', fallback: ['dlg', 'dlg_dep'])
dep_swa = dependency('swa',
	fallback: ['swa', 'swa_dep'],
	default_options: [
		'with-gl=disabled',
		'link-vulkan=false',
		'with-kms=disabled'
	])
dep_vkpp = dependency('vkpp MASTER',
	fallback: ['vkpp', 'vkpp_dep'],
	default_options: ['lib=false'])

src = files(
	# own meta sources & util
	'src/layer.cpp',
	'src/data.cpp',
	'src/window.cpp',
	'src/util.cpp',
	'src/f16.cpp',
	'src/api.cpp',
	'src/commands.cpp',
	'src/overlay.cpp',

	# gui
	'src/gui/renderer.cpp',
	'src/gui/gui.cpp',
	'src/gui/resources.cpp',
	'src/gui/image.cpp',
	'src/gui/buffer.cpp',
	'src/gui/cb.cpp',

	# vulkan api entrypoints
	'src/handle.cpp',
	'src/device.cpp',
	'src/swapchain.cpp',
	'src/image.cpp',
	'src/sync.cpp',
	'src/rp.cpp',
	'src/cb.cpp',
	'src/ds.cpp',
	'src/buffer.cpp',
	'src/memory.cpp',
	'src/shader.cpp',
	'src/pipe.cpp',

	# external util
	'src/spirv_reflect.c',

	# useful for visual studio build to include them here
	'src/fwd.hpp',
	'src/util.hpp',
	'src/data.hpp',
	'include/api.h',
	# TODO: include other headers
)

inc = [
	include_directories('src'),
	include_directories('include'),
]

deps = [
	dep_threads,
	dep_dlg,
	dep_vkpp,
	dep_swa,
	libimgui_core_dep,
]

vklayer_mesa_overlay = shared_library(
	'VkLayer_fuencaliente',
	[src, spv],
	gnu_symbol_visibility: 'hidden',
	dependencies: deps,
	include_directories: inc,
	link_args: cc.get_supported_link_arguments(['-Wl,-Bsymbolic-functions', '-Wl,-z,relro']),
	install: true
)

install_data(
	files('VkLayer_fuencaliente.json'),
	install_dir : join_paths(get_option('datadir'), 'vulkan', 'explicit_layer.d'),
)
