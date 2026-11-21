# Wayland hooked overlay

When using a non-integrated overlay (via `VIL_HOOK_OVERLAY=1`) on wayland,
there are multiple options how the overlay should behave. Unlike
other platforms, core wayland does not allow to steal input from a surface
but has some nice alternatives (depending on your compositor).
In either case, vil creates its own window and does not render directly into
the same swapchain the application uses.

Recommended: Set the `VIL_WLR_OVERLAY=1` environment variable. When
the wlr_overlay protocol is supported by the compositor, vil will create the
overlay window with wlr_layer role on the overlay layer.
The start tab in the vil UI then offers some additional options.
Rather than an in-window overlay, this will create a global overlay
but I found this to be useful enough.

Second option: Set `VIL_OVERLAY_WL_CHILD=1`. This requires the
xdg-foreign-v2 protocol and will simply create the overlay window
as child of the application window. How useful this is depends on the
compositor (I tested it with `niri` where it wasn't very useful).
TODO: maybe we can make this more useful using the `positioner` protocol?

When none of these options is set or the set option is not supported,
the overlay will simply show up in a normal window. Toggling it is
still possible with the overlay hotkey.

external overlay with core protocol isn't really possible:
- we could register for input
- we can render into the window
- however: we cannot block input from reaching the window
  overlay becomes useless with that
	- could use wayland.app/protocols/xwayland-keyboard-grab-unstable-v1.
	  terrible and hacky tho

## Remaining issues

This only works with a single application window and makes a lot of
assumptions. See the todos in wayland.cpp:keyboardKey callback.

## TODO: improve Clean integration with swa

Just add custom features to swa (main) via extensions.
Allow to chain extensions to window_settings and display creation.
They can then trigger additional behavior.
Also add custom utility functions to swa/wayland.h that allows layer
shell manipulation (without having to use the protocol directly in vil).
