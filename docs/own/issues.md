# Known issues

Using resource aliasing might result in vil reading out aliased, deactivated
resources. It usually just shows garbage (which makes sense) but strictly
speaking (per Vulkan spec) this is undefined behaviors and *might*
cause issues on some drivers. No driver on which this actually has problematic
side effects (such as crashing/memory corruption) are known to me at the moment.
