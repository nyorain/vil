# FAQ and known issues

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

