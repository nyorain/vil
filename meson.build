project('fuencaliente', ['c', 'cpp'],
	version: '0.1.0',
	default_options: [
		'werror=true',
		'warning_level=3',
		'cpp_std=c++17',
		'c_std=c11',
	]
)

subdir('src/data') # compile shaders

cc = meson.get_compiler('cpp')

args = cc.get_supported_arguments([
	# silence various msvc warnings, we still wanna compile with werror
	'/wd4065', # switch has no case, just default
	'/wd26812', # prefer 'enum class' over 'enum'
	'/wd4244', # lossy type conversion (e.g. double -> int)
	'/wd4305', # truncating type conversion (e.g. double -> float)
	'/wd4127', # conditional expression is constant (might be useful but not really for us)
	'/wd4456', # delecaration hides previous local declaration (sorry, too used to rust's beauty)

	# TODO: msvc slow as fuck in debug build.
	'-D_ITERATOR_DEBUG_LEVEL=0',

	# TODO: those should be fixed in code, really.
	'/wd4458', # declaration hides class member
])

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

with_x11 = true

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
	'src/vk/format_utils.cpp',
	'src/platform.cpp',

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
	'src/queryPool.cpp',

	# external util
	'src/spirv_reflect.c',

	# useful for visual studio build to include headers here
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
	'src/imgui/imgui.h',
	'include/fuen_api.h',
)

if with_x11
	src += files('src/xlib.cpp')
endif

public_inc = include_directories('include/')
private_inc = include_directories('src/')
shader_inc = include_directories('src/data/')

inc = [
	public_inc,
	private_inc,
	shader_inc,
]

deps = [
	dep_threads,
	dep_dlg,
	dep_swa,
	libimgui_core_dep,
	dep_vulkan,
]

if with_x11
	dep_xcb = dependency('xcb', required: true)
	dep_xcb_keysyms = dependency('xcb-keysyms', required: true)
	dep_x11_xcb = dependency('x11-xcb', required: true)

	deps += [dep_xcb, dep_xcb_keysyms, dep_x11_xcb]
endif

fuen_layer = shared_library(
	'VkLayer_fuencaliente',
	[src, spv],
	gnu_symbol_visibility: 'hidden',
	dependencies: deps,
	include_directories: inc,
	# link_args: cc.get_supported_link_arguments(['-Wl,-Bsymbolic-functions', '-Wl,-z,relro']),
	cpp_args: args,
	install: true
)

conf_data = configuration_data()
conf_data.set('fuen_version', meson.project_version())

# TODO: this is terrible... needed for windows
libpath = fuen_layer.full_path()
if libpath.contains('\\')
	conf_data.set('lib_path', '.\\\\' + libpath.split('\\')[-1])
else
	conf_data.set('lib_path', libpath.split('/')[-1])
endif

configure_file(
	input: 'VkLayer_fuencaliente.json.in',
	output: 'VkLayer_fuencaliente.json',
	install: true,
	install_dir: join_paths(get_option('datadir'), 'vulkan', 'explicit_layer.d'),
	configuration: conf_data
)

subdir('docs/example')
subdir('docs/test')
