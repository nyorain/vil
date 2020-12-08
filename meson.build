project('fuencaliente', ['c', 'cpp'],
	version: '0.1.0',
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

dep_threads = dependency('threads', required: false)
dep_dlg = dependency('dlg', fallback: ['dlg', 'dlg_dep'])
dep_swa = dependency('swa',
	fallback: ['swa', 'swa_dep'],
	default_options: [
		'with-gl=disabled',
		'link-vulkan=false',
		'with-kms=disabled'
	])

# the same way it's done in swa when not linking vulkan:
# we only want the headers and definitions, not link anything.
# we currently carefully only use the header-only vk_layer utilities
dep_vulkan_full = dependency('vulkan')
dep_vulkan = dep_vulkan_full.partial_dependency(
	compile_args: true,
	includes: true)

subdir('src/imgui')

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
	'src/device.hpp',
	'src/layer.hpp',
	'src/cb.hpp',
	'src/handles.hpp',
	'src/commands.hpp',
	'src/rp.hpp',
	'src/sync.hpp',
	'src/pipe.hpp',
	'src/shader.hpp',
	'src/ds.hpp',
	'src/window.hpp',
	'src/overlay.hpp',
	'src/gui/gui.hpp',
	'src/gui/resources.hpp',
	'src/gui/cb.hpp',
	'src/gui/renderer.hpp',
	'src/util.hpp',
	'src/data.hpp',
	'include/fuen_api.h',
)

public_inc = include_directories('include')
private_inc = include_directories('src')
inc = [
	public_inc,
	private_inc,
]

deps = [
	dep_threads,
	dep_dlg,
	dep_swa,
	libimgui_core_dep,
]

fuen_layer = shared_library(
	'VkLayer_fuencaliente',
	[src, spv],
	gnu_symbol_visibility: 'hidden',
	dependencies: deps,
	include_directories: inc,
	link_args: cc.get_supported_link_arguments(['-Wl,-Bsymbolic-functions', '-Wl,-z,relro']),
	install: true
)

conf_data = configuration_data()
conf_data.set('fuen_version', meson.project_version())
conf_data.set('lib_path', fuen_layer.full_path().split('/')[-1])

configure_file(
	input: 'VkLayer_fuencaliente.json.in',
	output: 'VkLayer_fuencaliente.json',
	install: true,
	install_dir: join_paths(get_option('datadir'), 'vulkan', 'explicit_layer.d'),
	configuration: conf_data
)

subdir('docs/example')
