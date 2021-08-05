project('VIL', ['c', 'cpp'],
	version: '0.1.0',
	default_options: [
		'werror=true',
		'warning_level=3',
		'cpp_std=c++17',
		# NOTE: this will cause issues on old msvc versions, with new
		# meson versions. You need at least visual studio 19.28, see
		# https://github.com/mesonbuild/meson/pull/7967 (already in the last
		# couple meson relases)
		'c_std=c11',
		# We need this to build the subprojects as static libraries. No
		# other way to do this apparently.
		# And we force the layer to be shared library anyways.
		'default_library=static',
	]
)

shader_inc = []
subdir('src/data') # compile shaders

cc = meson.get_compiler('cpp')

args = cc.get_supported_arguments([
	# silence various msvc warnings, we still wanna compile with werror
	'/wd4065', # switch has no case, just default
	'/wd26812', # prefer 'enum class' over 'enum'. Warning isn't wrong but can't change external code
	'/wd4244', # lossy type conversion (e.g. double -> int)
	'/wd4305', # truncating type conversion (e.g. double -> float)
	'/wd4127', # conditional expression is constant (might be useful but not really for us)
	'/wd4456', # delecaration hides previous local declaration (sorry, too used to rust's beauty)
	'/wd4458', # declaration hides class member, TODO: should be fixed in code, valid warning
	'/wd5105', # needed to fix an issue in winbase.h on a specific windows version (wtf microsoft)
	'/wd4267', # lossy conversion warning, way too strict.
	'/wd4245', # signed/unsigned warning, way too strict.
	'-D_CRT_SECURE_NO_WARNINGS',

	# msvc really slow in debug build without this
	#'-D_ITERATOR_DEBUG_LEVEL=0',

	# for clang
	'-Wno-missing-braces',

	# to be compatible with msvc warning level on gcc/clang
	# debatable whether the warnings are actually useful
	# '-Wconversion',
	# '-Wno-float-conversion',
])

# defines
args += [
	'-DSPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS=1',
	'-DSPIRV_CROSS_NAMESPACE_OVERRIDE=spc',
]

if get_option('buildtype') == 'debug'
	# highly useful for debugging but has impact on performance
	args += '-D_GLIBCXX_DEBUG'
endif

# TODO: yeah, this needs some love
with_win32 = build_machine.system() == 'windows'

with_x11_hook = not with_win32 and get_option('x11-hook')

tracy_deps = []

with_tracy = get_option('tracy')

if with_tracy
	args += '-DTRACY_ENABLE'
	# we always want on-demand profiling
	args += '-DTRACY_ON_DEMAND'
	if build_machine.system() == 'windows'
		tracy_deps += cc.find_library('Ws2_32')
		tracy_deps += cc.find_library('Dbghelp')
	else
		tracy_deps += cc.find_library('dl')
	endif
endif

layer_args = [
	# Make sure to *never* include them
	'-DVK_NO_PROTOTYPES',
]

# config options
with_debug_checks = get_option('buildtype') != 'release'
if with_debug_checks
	# layer_args += '-DVIL_EXTENSIVE_ZONES'
	# layer_args += '-DVIL_TRACY_MUTEX'
	layer_args += '-DVIL_DEBUG_MUTEX'
	layer_args += '-DVIL_DEBUG'
endif

disable_dlg = false
if disable_dlg
	layer_args += '-DDLG_DISABLE'
endif

# deps
dep_threads = dependency('threads', required: false)
dep_dlg = dependency('dlg',
	static: true,
	fallback: ['dlg', 'dlg_dep'])
dep_swa = dependency('swa',
	fallback: ['swa', 'swa_dep'],
	static: true,
	default_options: [
		'with-gl=disabled',
		'link-vulkan=false',
		'with-kms=disabled'
	])

src = files(
	# own meta sources & util
	'src/layer.cpp',
	'src/data.cpp',
	'src/window.cpp',
	'src/api.cpp',
	'src/overlay.cpp',
	'src/platform.cpp',
	'src/threadContext.cpp',
	'src/vk/format_utils.cpp',
	'src/util/util.cpp',
	'src/util/f16.cpp',
	'src/util/ext.cpp',
	'src/util/ownbuf.cpp',
	'src/util/buffmt.cpp',
	'src/util/bufparser.cpp',
	'src/command/alloc.cpp',
	'src/command/desc.cpp',
	'src/command/record.cpp',
	'src/command/commands.cpp',

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
	'src/queue.cpp',
	'src/submit.cpp',
	'src/accelStruct.cpp',

	# gui stuff
	'src/gui/render.cpp',
	'src/gui/gui.cpp',
	'src/gui/resources.cpp',
	'src/gui/cb.cpp',
	'src/gui/command.cpp',
	'src/gui/commandHook.cpp',
	'src/gui/vertexViewer.cpp',
	'src/gui/bufferViewer.cpp',
	'src/gui/imageViewer.cpp',
	'src/gui/blur.cpp',

	# imgui
	'src/imgui/imgui.cpp',
	'src/imgui/imgui_draw.cpp',
	'src/imgui/imgui_widgets.cpp',
	'src/imgui/imgui_demo.cpp',
	'src/imgui/imgui_tables.cpp',
	'src/imgui/textedit.cpp',

	# useful for visual studio build to include headers here
	'src/fwd.hpp',
	'src/device.hpp',
	'src/layer.hpp',
	'src/cb.hpp',
	'src/handles.hpp',
	'src/rp.hpp',
	'src/accelStruct.hpp',
	'src/sync.hpp',
	'src/pipe.hpp',
	'src/shader.hpp',
	'src/ds.hpp',
	'src/window.hpp',
	'src/overlay.hpp',
	'src/queue.hpp',
	'src/platform.hpp',
	'src/win32.hpp',
	'src/data.hpp',
	'src/queryPool.hpp',
	'src/submit.hpp',
	'src/threadContext.hpp',
	'src/command/commands.hpp',
	'src/command/record.hpp',
	'src/command/alloc.hpp',
	'src/command/desc.hpp',

	'src/tracy/TracyClient.cpp',

	'src/vk/vulkan.h',
	'src/vk/vulkan_core.h',
	'src/vk/vulkan_win32.h',
	'src/vk/typemap_helper.h',
	'src/vk/dispatch_table.h',
	'src/vk/dispatch_table_helper.h',
	'src/vk/vk_layer.h',
	'src/vk/format_utils.h',

	'src/gui/gui.hpp',
	'src/gui/resources.hpp',
	'src/gui/cb.hpp',
	'src/gui/command.hpp',
	'src/gui/render.hpp',
	'src/gui/util.hpp',
	'src/gui/commandHook.hpp',
	'src/gui/vertexViewer.hpp',
	'src/gui/blur.hpp',

	# fonts
	'src/gui/fonts.cpp',

	'src/util/util.hpp',
	'src/util/span.hpp',
	'src/util/flags.hpp',
	'src/util/f16.hpp',
	'src/util/intrusive.hpp',
	'src/util/syncedMap.hpp',
	'src/util/ext.hpp',
	'src/util/debugMutex.hpp',
	'src/util/profiling.hpp',
	'src/util/spirv.hpp',
	'src/util/camera.hpp',
	'src/util/bytes.hpp',
	'src/util/ownbuf.hpp',
	'src/util/buffmt.hpp',

	'include/vil_api.h',
	'src/imgui/imgui.h',

	# spirv-cross
	'src/spirv-cross/spirv.h',
	'src/spirv-cross/spirv.hpp',
	'src/spirv-cross/spirv_cfg.hpp',
	'src/spirv-cross/spirv_common.hpp',
	'src/spirv-cross/spirv_cross.hpp',
	'src/spirv-cross/spirv_cross_containers.hpp',
	'src/spirv-cross/spirv_cross_error_handling.hpp',
	'src/spirv-cross/spirv_cross_parsed_ir.hpp',
	'src/spirv-cross/spirv_parser.hpp',

	'src/spirv-cross/spirv_cfg.cpp',
	'src/spirv-cross/spirv_cross.cpp',
	'src/spirv-cross/spirv_parser.cpp',
	'src/spirv-cross/spirv_cross_parsed_ir.cpp',
)

with_tests = get_option('tests')
if with_tests
	src += files(
		'src/test/bugged.cpp',
		'src/test/rpsplit.cpp',
		'src/test/xfb.cpp',
		'src/test/bufferAddress.cpp',
		'src/test/bufparse.cpp',
	)
endif

public_inc = include_directories('include/')
private_inc = include_directories('src/')

inc = [
	public_inc,
	private_inc,
	shader_inc,
]

deps = [
	dep_threads,
	dep_dlg,
	dep_swa,
	tracy_deps,
]

# TODO, wip, just for testing backtraces
if not with_win32
	dep_dw = dependency('libdw', required: false)
	if dep_dw.found()
		deps += dep_dw
		layer_args += '-DBACKWARD_HAS_DW'
	endif
endif

if with_x11_hook
	src += files('src/xlib.cpp')

	dep_xcb = dependency('xcb', required: true)
	dep_xcb_keysyms = dependency('xcb-keysyms', required: true)
	dep_x11_xcb = dependency('x11-xcb', required: true)

	deps += [dep_xcb, dep_xcb_keysyms, dep_x11_xcb]
	args += ['-DVIL_WITH_X11']
endif

if with_win32
	src += files('src/win32.cpp')
	args += ['-DVIL_WITH_WIN32']
endif

# On win32, pch breaks with tracy due to some windows.h bs.
# windows.h keeps breaking my applications and will to live
pch = []
if not with_win32 or not with_tracy
	pch = 'src/pch.hpp'
endif

vil_layer = shared_library(
	'VkLayer_live_introspection',
	[src, spv],
	gnu_symbol_visibility: 'hidden',
	dependencies: deps,
	include_directories: inc,
	cpp_args: args + layer_args,
	install: true,
	cpp_pch: pch,
)

conf_data = configuration_data()
conf_data.set('vil_version', meson.project_version())

# TODO: this is terrible... needed for windows
libpath = vil_layer.full_path()
if libpath.contains('\\')
	conf_data.set('lib_path', '.\\\\' + libpath.split('\\')[-1])
else
	conf_data.set('lib_path', libpath.split('/')[-1])
endif

configure_file(
	input: 'VkLayer_live_introspection.json.in',
	output: 'VkLayer_live_introspection.json',
	install: true,
	install_dir: join_paths(get_option('datadir'), 'vulkan', 'explicit_layer.d'),
	configuration: conf_data
)

if build_machine.system() == 'windows'
	# TODO add as install script via meson.add_install_script?
	configure_file(
		input: 'register_layer.bat.in',
		output: 'register_layer.bat',
		install: false,
		copy: true,
	)
endif

if get_option('example')
	subdir('docs/example')
endif

if with_tests
	viltest = executable('viltest', files('src/test/main.cpp'),
		include_directories: inc,
		cpp_args: args,
		dependencies: dep_dlg,
		link_with: vil_layer)
	test('viltest', viltest)
endif
