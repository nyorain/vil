project('VIL', ['c', 'cpp'],
	version: '0.1.0',
	default_options: [
		# NOTE: werror is a good idea in theory but there are too many
		# compiler versions out there and people building it having
		# to fight with build errors (instead of warnings) is not worth it.
		# Gets manually activated for CI.
		# 'werror=true',
		'werror=false',
		'swa:werror=false',
		'imgio:werror=false',
		'nytl:werror=false',
		'spc:werror=false',
		'pml:werror=false',
		'dlg:werror=false',
		'png:werror=false',
		'vkmockicd:werror=false',
		'zlib:werror=false',

		'warning_level=3',
		'cpp_std=c++17',
		# NOTE: this will cause issues on old msvc versions, with new
		# meson versions. You need at least visual studio 19.28, see
		# https://github.com/mesonbuild/meson/pull/7967 (already in the last
		# couple meson relases)
		'c_std=c11',
		# We need this to build the subprojects as static libraries. No
		# other way to do this apparently.
		# And we force the layer to be a shared library anyways.
		'default_library=static',
	]
)

# Options

with_tracy = get_option('tracy')
with_window = get_option('window')
with_example = get_option('example')
is_release = (get_option('buildtype') == 'release')
with_standalone = get_option('with-standalone')
with_callstacks = get_option('with-callstacks')

profiling = with_tracy
debug_checks = not is_release
disable_dlg = is_release
extensive_zones = profiling
tracy_mutex = profiling
debug_stats = true

###############################################################

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
	'/wd4201', # nameless struct/union
	'-D_CRT_SECURE_NO_WARNINGS',

	# msvc really slow in debug build without this
	#'-D_ITERATOR_DEBUG_LEVEL=0',

	# NOTE: this is super useful to optimize compile times, with clang
	# See https://www.phoronix.com/news/LLVM-Clang-9.0-Time-Trace
	# View e.g. via ui.perfetto.dev. The trace files will be in the build dir,
	# right next to the object files
	'-ftime-trace',

	# for clang
	'-Wno-missing-braces',
	'-Wno-newline-eof', # needed for spvm TODO re-enable and fix!
	'-Wno-nonnull-compare', # seems like clang bug

	# gcc bug for optimized builds
	'-Wno-error=array-bounds',

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

if with_tracy
	args += '-DTRACY_ENABLE'
	# we always want on-demand profiling
	args += '-DTRACY_ON_DEMAND'
	args += '-DTRACY_NO_FRAME_IMAGE'
	args += '-DTRACY_DELAYED_INIT'

	# Without this we get problems in same applications.
	# We manually init/shutdown on instance create/destroy
	args += '-DTRACY_MANUAL_LIFETIME'

	if build_machine.system() == 'windows'
		tracy_deps += cc.find_library('Ws2_32')
		tracy_deps += cc.find_library('Dbghelp')
	else
		tracy_deps += cc.find_library('dl')
	endif
endif

layer_args = cc.get_supported_arguments([
	# Make sure to *never* include them
	'-DVK_NO_PROTOTYPES',

	# for imgui, C++20
	# TODO: only disable werror?
	'-Wno-deprecated-enum-enum-conversion',
])

if debug_stats
	layer_args += '-DVIL_DEBUG_STATS'
endif

if extensive_zones
	layer_args += '-DVIL_EXTENSIVE_ZONES'
endif

if tracy_mutex
	layer_args += '-DVIL_TRACY_MUTEX'
endif

if debug_checks
	layer_args += '-DVIL_DEBUG_MUTEX'
	layer_args += '-DVIL_DEBUG'
endif

# deps
dep_threads = dependency('threads', required: false)

dep_dlg = dependency('dlg',
	static: true,
	fallback: ['dlg', 'dlg_dep'])
dep_nytl = dependency('nytl',
	fallback: ['nytl', 'nytl_dep'])
dep_imgio = dependency('imgio',
	static: true,
	default_options: ['png=false'],
	fallback: ['imgio', 'imgio_dep'])

layer_args += '-DDLG_DEFAULT_TAGS="vil"'

if disable_dlg
	warning('disabling dlg')

	layer_args += '-DDLG_DISABLE=1'

	layer_args += cc.get_supported_arguments([
		'/wd4100', # unreferenced formal parameter.
		'/wd4189', # unused local variable
		'-Wno-unused-parameter',
		'-Wno-unused-but-set-variable',
		'-Wno-unused-variable',
	])
elif get_option('throw-on-assert')
	layer_args += '-DVIL_THROW_ON_ASSERT'
endif

# spvm
spvm_src = files(
	'src/spvm/context.c',
	'src/spvm/result.c',
	'src/spvm/state.c',
	'src/spvm/types.c',
	'src/spvm/value.c',
	'src/spvm/image.c',
	'src/spvm/program.c',
	'src/spvm/opcode_setup.c',
	'src/spvm/opcode_execute.c',
	'src/spvm/ext/GLSL450.c',
)

src = files(
	# own meta sources & util
	'src/layer.cpp',
	'src/data.cpp',
	'src/api.cpp',
	'src/overlay.cpp',
	'src/platform.cpp',
	'src/lmm.cpp',
	'src/fault.cpp',
	'src/util/util.cpp',
	'src/util/fmt.cpp',
	'src/util/f16.cpp',
	'src/util/ext.cpp',
	'src/util/ownbuf.cpp',
	'src/util/buffmt.cpp',
	'src/util/bufparser.cpp',
	'src/util/linalloc.cpp',
	'src/command/match.cpp',
	'src/command/record.cpp',
	'src/command/commands.cpp',
	'src/command/builder.cpp',

	'src/serialize/serialize.cpp',
	'src/serialize/commands.cpp',
	'src/serialize/handles.cpp',

	# vulkan api entrypoints
	'src/handle.cpp',
	'src/device.cpp',
	'src/swapchain.cpp',
	'src/image.cpp',
	'src/imageLayout.cpp',
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
	'src/gui/pipes.cpp',
	'src/gui/resources.cpp',
	'src/gui/cb.cpp',
	'src/gui/command.cpp',
	'src/gui/vertexViewer.cpp',
	'src/gui/bufferViewer.cpp',
	'src/gui/imageViewer.cpp',
	'src/gui/shader.cpp',
	'src/gui/blur.cpp',
	'src/gui/commandSelection.cpp',
	# debug wip
	'src/gui/vizlcs.cpp',

	'src/commandHook/hook.cpp',
	'src/commandHook/record.cpp',
	'src/commandHook/submission.cpp',
	'src/commandHook/copy.cpp',

	# vulkan and util
	'src/vk/format_utils.cpp',
	'src/vkutil/sync.cpp',
	'src/vkutil/cmd.cpp',
	'src/vkutil/handles.cpp',
	'src/vkutil/dynds.cpp',

	# useful for visual studio build to include headers here
	'src/fwd.hpp',
	'src/device.hpp',
	'src/layer.hpp',
	'src/cb.hpp',
	'src/rp.hpp',
	'src/accelStruct.hpp',
	'src/image.hpp',
	'src/imageLayout.hpp',
	'src/sync.hpp',
	'src/pipe.hpp',
	'src/shader.hpp',
	'src/ds.hpp',
	'src/overlay.hpp',
	'src/queue.hpp',
	'src/platform.hpp',
	'src/win32.hpp',
	'src/data.hpp',
	'src/queryPool.hpp',
	'src/submit.hpp',
	'src/frame.hpp',
	'src/threadContext.hpp',
	'src/fault.hpp',
	'src/command/commands.hpp',
	'src/command/record.hpp',
	'src/command/alloc.hpp',
	'src/command/match.hpp',

	'src/serialize/serialize.hpp',
	'src/serialize/internal.hpp',
	'src/serialize/util.hpp',
	'src/serialize/bufs.hpp',

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
	'src/gui/vertexViewer.hpp',
	'src/gui/imageViewer.hpp',
	'src/gui/shader.hpp',
	'src/gui/blur.hpp',
	'src/gui/commandSelection.hpp',
	'src/gui/vizlcs.hpp',

	'src/commandHook/hook.hpp',
	'src/commandHook/record.hpp',
	'src/commandHook/submission.hpp',
	'src/commandHook/state.hpp',
	'src/commandHook/copy.hpp',

	# fonts
	'src/gui/fonts.cpp',

	'src/util/util.hpp',
	'src/util/fmt.hpp',
	'src/util/f16.hpp',
	'src/util/intrusive.hpp',
	'src/util/syncedMap.hpp',
	'src/util/ext.hpp',
	'src/util/debugMutex.hpp',
	'src/util/profiling.hpp',
	'src/util/spirv.hpp',
	'src/util/camera.hpp',
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

imgui_src = files(
	# imgui
	'src/imgui/imgui.cpp',
	'src/imgui/imgui_draw.cpp',
	'src/imgui/imgui_widgets.cpp',
	'src/imgui/imgui_demo.cpp',
	'src/imgui/imgui_tables.cpp',
	'src/imgui/textedit.cpp',
)

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
	dep_nytl,
	dep_imgio,
	tracy_deps,
]

dep_swa = []
# TODO: add extra option whether to enable overlay on win32
# also, even with overlay, we shouldn't pull swa as dependency.
# shold probably just copy the key_to_winapi function
if with_window or with_x11_hook or with_win32 or with_example
	dep_swa = dependency('swa',
		fallback: ['swa', 'swa_dep'],
		static: true,
		default_options: [
			'with-gl=disabled',
			'link-vulkan=false',
			'with-kms=disabled'
		])

	if with_window or with_x11_hook or with_win32
		deps += dep_swa
		src += files(
			'src/window.cpp',
			'src/swaPlatform.cpp',

			'src/window.hpp',
			'src/swaPlatform.hpp',
		)

		layer_args += '-DVIL_WITH_SWA=1'
	endif
endif

with_unit_tests = get_option('unit-tests')
if with_unit_tests
	src += files(
		'src/test/bugged.cpp',
		'src/test/unit/entry.cpp',
		'src/test/unit/rpsplit.cpp',
		'src/test/unit/xfb.cpp',
		'src/test/unit/bufferAddress.cpp',
		'src/test/unit/bufparse.cpp',
		'src/test/unit/match.cpp',
		'src/test/unit/lmm.cpp',
		'src/test/unit/fmt.cpp',
		'src/test/unit/imageLayout.cpp',
	)
endif

with_integration_tests = get_option('integration-tests')
if with_integration_tests
	src += files(
		'src/test/integration/entry.cpp',
		'src/test/integration/internal.cpp', # TODO: rename
		'src/test/integration/gui.cpp',
	)
endif

if with_standalone
	src += files(
		'src/standalone/entry.cpp',
		'src/util/dl.cpp',
	)
	# needed for dl.cpp dlopen
	deps += dependency('dl', required: false)
endif

if with_callstacks
	layer_args += '-DVIL_COMMAND_CALLSTACKS'
	src += files(
		'src/backward/trace.cpp',
		'src/backward/resolve.cpp',

		'src/backward/trace.hpp',
		'src/backward/resolve.hpp',
		'src/backward/common.hpp',
	)
endif

# TODO, wip, just for testing backtraces
if not with_win32 and get_option('libdw')
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
	src += files(
		'src/win32.cpp',

		'src/minhook/buffer.c',
		'src/minhook/hook.c',
		'src/minhook/trampoline.c',
		# We currently only support 64 bit
		# 'src/minhook/hde/hde32.c',
		'src/minhook/hde/hde64.c',
		'src/minhook/MinHook.h',
		'src/minhook/buffer.h',
		'src/minhook/trampoline.h',
	)
	args += ['-DVIL_WITH_WIN32']
	args += ['/wd4310'] # for minhook
endif

# On win32, pch breaks with tracy due to some windows.h bs.
# windows.h keeps breaking my applications and will to live
pch = []
if not with_win32 or not with_tracy
	# TODO: fix. We want to use pch unless we are frequently editing shaders
	# See https://github.com/mesonbuild/meson/issues/10882
	pch = 'src/pch.hpp'
endif

lib_spvm = static_library(
	'spvm',
	spvm_src,
	gnu_symbol_visibility: 'hidden',
	include_directories: inc,
	c_args: args + cc.get_supported_arguments([
		'-Wno-unused-parameter',
		'-Wno-unused-variable',
		'/wd4100', # unreferenced formal parameter.
		'/wd4189', # unused local variable
		'/wd4457', # declaration hides function parameter
		# TODO: fix the ones below?
		'-Wno-sign-compare',
		# new clang warnings
		'-Wno-strict-prototypes',
		'-Wno-unused-but-set-parameter',
		'-Wno-unused-but-set-variable',
		'/wd4018', # signed/unsigned mismatch
		'/wd4090', # different 'const' qualifier on free/realloc calls
	]),
)

# building as separate lib as ugly workaround for gcc pch warning issue
# See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=64117
lib_imgui = static_library(
	'imgui',
	imgui_src,
	gnu_symbol_visibility: 'hidden',
	include_directories: inc,
	cpp_args: layer_args + args,
)

link_args = [
	# statically link the standard library, where supported
	# might get conflicts with the one loaded by the application otherwise
	# TODO: nope, using this causes weird runtime issues on linux (dlg::format
	# not working as expected anymore?!)
	# '-static-libstdc++',
]

if get_option('b_coverage')
	link_args += ['--coverage']
endif

vil_layer = shared_library(
	'VkLayer_live_introspection',
	sources: [src + spv],
	gnu_symbol_visibility: 'hidden',
	dependencies: deps,
	include_directories: inc,
	link_whole: [lib_spvm, lib_imgui],
	cpp_args: args + layer_args,
	c_args: args,
	link_args: cc.get_supported_link_arguments(link_args),
	install: true,
	cpp_pch: pch,
)

conf_data = configuration_data()
conf_data.set('vil_version', meson.project_version())

libpath = vil_layer.full_path()
json_config_absolute_path = get_option('json-config-absolute-path')

# TODO: this is terrible... needed for windows
if libpath.contains('\\')
	if not json_config_absolute_path
		libpath = libpath.split('\\')[-1]
	endif
	conf_data.set('lib_path', '.\\\\' + libpath)
else
	if not json_config_absolute_path
		libpath = libpath.split('/')[-1]
	endif
	conf_data.set('lib_path', libpath)
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

if with_example
	subdir('docs/example')
endif

if with_unit_tests
	# executor for embedded unit tests
	viltest = executable('viltest', files('src/test/unit/main.cpp'),
		include_directories: inc,
		cpp_args: args,
		dependencies: [],
		link_with: vil_layer)
	test('viltest', viltest)
endif

if with_integration_tests
	# integration tests
	dep_vulkan = dependency('vulkan')
	dep_dl = cc.find_library('dl', required: false)

	mock_icd_sub = subproject('vkmockicd')
	mock_icd_file = mock_icd_sub.get_variable('icd_json_file')
	mock_icd_lib = mock_icd_sub.get_variable('lib')

	int_src = files(
		'src/test/bugged.cpp',
		'src/test/integration/main.cpp',
		'src/test/integration/raw.cpp',
	)

	int_args = args
	int_args += '-DVIL_MOCK_ICD_FILE="@0@"'.format(mock_icd_file)
	int_args += '-DVIL_LAYER_PATH="@0@"'.format(meson.current_build_dir())

	intest = executable('intest', [int_src, mock_icd_lib],
		include_directories: inc,
		cpp_args: int_args,
		dependencies: [dep_dlg, dep_vulkan, dep_dl, dep_nytl])
	test('intest', intest)
endif

# standalone
if with_standalone
	iv = executable('iv', files('src/standalone/main.cpp'),
		include_directories: inc,
		cpp_args: args,
		dependencies: [],
		link_with: vil_layer)
endif
