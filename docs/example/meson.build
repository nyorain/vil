# expects that the layer was installed
# TODO: add test target where we just adapt the environment variable?

lib_dl = cc.find_library('dl', required: false)
executable('fuen-example',
	files('main.c'),
	include_directories: public_inc,
	dependencies: [dep_dlg, dep_threads, dep_vulkan_full, dep_swa, lib_dl])
