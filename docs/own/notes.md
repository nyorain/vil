- layer api doc: https://vulkan.lunarg.com/doc/sdk/1.2.154.1/windows/loader_and_layer_interface.html

---

I'm not entirely sure whether we can assume e.g. `vkCmdDrawIndirectCountAMD`
is available when `vkCmdDrawIndirectCount` (core 1.2) is. The vulkan spec
is really confusing on this topic. Found a relevant issue:

https://github.com/KhronosGroup/Vulkan-Docs/issues/682

The talk in there seems like it is not. The old non-core extension does
not have to be exposed when vulkan 1.2 is used. But maybe the loader
does the magic for the aliases and we can retrieve it via vkGetDeviceProcAddr
nonetheless? I'm not sure. For now, we should use our `selectCmd` stuff everywhere.

---

Adding a `Command* parent` to Command would make a lot of things easier
in terms of hierarchy traversal. But we can't to that, mainly because
of secondary command buffers, as they are effectively inlined.

---

last commit with alternative (linked-list, self-maintained heap) versions
of candidate management in lmm:
de8321e56ec94465f0bfa39d20da179a7ac84ac7

