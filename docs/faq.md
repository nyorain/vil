# FAQ and known issues

Limitations, features planned for later:
- Support for multiple swapchains.
  When you have more than one swapchain, vil might act weird.
- Support for sparse memory bindings
- (full) support for aliased memory.
  Note that most things work but you may encounter some weird counter cases,
  vil isn't completely spec conforming for this feature, e.g. allows you
  to view invalidated images and buffers
- multi gpu support
	- just having multiple devices might vil make act weird
	- applications using device groups (core vulkan 1.1) are not supported

If you need any of those features, let us know your usecase. But please
understand that most of these features currently have low priority and
issues/crashes caused by your application using them are not considered bugs.

### Crash inside the validation layer when using vil

This can have multiple causes.

First of all, try to update the validation layer to the most recent version.
For older validation layer versions, try use `VIL_TIMELINE_SEMAPHORES=0` to 
disable the usage of timeline semaphores in vil as there used to be a critical error
in the validation layer regarding them.
Otherwise open an issue at https://github.com/nyorain/vil.

### Shader debugger doesn't show source code

Your SPIR-V compiler has to embed the original source code into the
binary. `glslangValidator` does this with `-g`. You should also make
sure to disable optimization with `-Od` otherwise debugging won't work
correctly.

The shader debugger is currently only tested with `glslang` output.

### Shader debugger can't handle `#line`

If your shader code (e.g. GLSL) contains a `#line` macro, the matching
between the current instruction and code line during SPIR-V debugging
will be off.
`glslang` should be used with `--target-env vulkan11` (or greater),
otherwise it will add a preamble to the embedded source code with a
line macro.

