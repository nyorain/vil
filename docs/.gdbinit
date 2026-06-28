# file /home/jan/code/iro/build/npt
#set args ~/art/assets/marble-bust/marble_bust_01_4k.gltf

file gnome-system-monitor

# file vkcube
# unset environment WAYLAND_DISPLAY

set environment VK_INSTANCE_LAYERS=VK_LAYER_live_introspection
set environment VIL_HOOK_OVERLAY=1
set environment VIL_WLR_OVERLAY=1
set environment VIL_CREATE_WINDOW=0
set environment VIL_ALLOW_UNSUPPORTED_EXTS=1

set debuginfod enabled off
cd build

# autoquit on application exit
set $_exitcode = -999
define hook-stop
	if $_exitcode != -999
		set confirm off
		quit
	end
end
