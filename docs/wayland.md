# Wayland UI

- external overlay isn't really possible
	- we could register for input
	- we can render into the window
	- however: we cannot block input from reaching the window
	  overlay becomes useless with that
		- idea: use wayland.app/protocols/xwayland-keyboard-grab-unstable-v1
		  terrible and hacky tho
- Spawning a window or using an application controlled overlay works normally
- For window we can use wlr-layer-shell to still have something like an overlay

While a proper overlay likely won't happen on wayland, we could probably
do a hybrid. Listen for input on the application surface to show/hide our
overlay window.

We don't even really need/require layer shell. Just use xdg_toplevel
with parenet otherwise. Can still do this useful hybrid. With positioner,
we can have the same toggles as with the layer shell (just inside the app window,
even better, probably). Could optionally still use layer-shell, maybe
decide via env var.

## Clean integration with swa

Just add custom features to swa (main) via extensions.
Allow to chain extensions to window_settings and display creation.
They can then trigger additional behavior.
Also add custom utility functions to swa/wayland.h that allows layer
shell manipulation (without having to use the protocol directly in vil).
