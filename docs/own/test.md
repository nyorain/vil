# Things we should test

Unit tests:
- useful error messages for the buffer parser.
  just look how it's done with pegtl for their json parser, could do
  it like that.

UI:
We should likely build an application that just does every command at
least once, even if non-sensical.
- all transfer commands in command viewer. The logic there is kinda ugly
  and some commands were never tested.

---

Writing own integration tests can be useful for reproducing edge cases
at times but we shouldn't spend too much time on it in general.
There are already great vulkan test suites we can use for the layer as well.
E.g. the Vulkan CTS (testing WIP) and the Vulkan validation layer tests.

## Validation layer tests

Especially the positive tests from the validation layers have proven
extremely useful, they found many subtle issues.
Current filter:

```
./tests/vk_layer_validation_tests --gtest_filter='VkPositiveLayerTest.*-VkPositiveLayerTest.CmdClearAttachmentTestsDynamicRendering:*Sparse*:*TwoInstances:*DestroySwapchainWithBoundImages:*ExplicitLayoutSpec:*ShaderAtomicFloat2:*ExternalSemaphore:*ExternalFence'
```

Some of them are in there because they crash my driver (radv, fall 2022)
and some because vil has no support/they are known issues (e.g. sparse memory, external sync, two instances).

---

As of december 2025, there are some additional steps needed to run the
validation layer tests. Especially `VK_ADD_LAYER_PATH` is needed, otherwise
the validation tests override the layer path and vil cannot be found.
I usually export:

```
VK_ADD_LAYER_PATH=./layers/
VK_INSTANCE_LAYERS=VK_LAYER_live_introspection

# vil configuration
VIL_DLG_HANDLER=1
VIL_CB_TEST_HOOK=1

# optional, to easier debug asserts
VIL_BREAK_ON_ERROR=1

# optional, to see *everything*
VIL_MIN_LOG_LEVEL=trace
```

## Proton, Wine, DXVK, VKD3D

Good tests for some advanced features.
Example command line:

```
VKD3D_CONFIG=no_staggered_submit
LD_PRELOAD=/usr/lib/libxkbcommon.so
PROTON_ENABLE_WAYLAND=0
DXVK_DEBUG=markers
VIL_DLG_HANDLER=1
VIL_LOG_FILE=/home/jan/vil-steam
VIL_WAIT_SURFACE=1
PROTON_DISABLE_NVAPI=1
VK_INSTANCE_LAYERS=VK_LAYER_live_introspection
VIL_CREATE_WINDOW=1
VIL_HOOK_OVERLAY=0
VIL_ALLOW_UNSUPPORTED_EXTS=1
PROTON_LOG=1
%command%
```

- no_staggered_submit for vkd3d is highly useful as tracking commands over
  multiple frames becomes very hard otherwise
- preloading of xkbcommon seems to be needed since wine/proton ships its
  own version that seems to cause issues. (ABI incompatible? old version? idk)
- VIL_WAIT_SURFACE seems to be needed, not sure why
- PROTON_DISABLE_NVAPI might fix some issues
- will create log files in homedir:
	- 'steam-$APPID' for the proton log
	- 'vil-steam' for the vil log

Useful: api dump. TODO: with newer proton versions, we need to redirect it to a file
```
VK_LUNARG_API_DUMP_PRE_DUMP=true
VK_INSTANCE_LAYERS=VK_LAYER_LUNARG_api_dump:VK_LAYER_live_introspection
```
