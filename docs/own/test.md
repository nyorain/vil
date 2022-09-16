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

Especially the positive tests from the validation layers have proven
extremely useful, they found many subtle issues.
Current filter:

```
./tests/vk_layer_validation_tests --gtest_filter='VkPositiveLayerTest.*-VkPositiveLayerTest.CmdClearAttachmentTestsDynamicRendering:*Sparse*:*TwoInstances:*DestroySwapchainWithBoundImages:*ExplicitLayoutSpec:*ShaderAtomicFloat2:*ExternalSemaphore:*ExternalFence'
```

Some of them are in there because they crash my driver (radv, fall 2022)
and some because vil has no support/they are known issues (e.g. sparse memory, external sync, two instances).
