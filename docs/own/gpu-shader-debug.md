# Shader debugging via GPU shader patching

Idea: instead of CPU emulation, do shader debugging directly on the GPU.
Patching the shaders to store state, where it is interesting.

Assumptions:
- shader/pipeline recompilation is fast. Like <1s
- For the beginning, we can just assume buffer device address support
  Saves us from the hustle with pipeline layouts, can pass the buffer dst
  address via specialization constant.
- we need to know/support all pipeline extensions to make this work,
  allow proper recompliation.
  SPIRV extensions are only a minor concern but *might* cause issues when
  not supported.

Flow:
- user selects shader debugging in UI
	- the shader source code is shown
- then, the user selects a line for a breakpoint.
  And can select the thread/vertex/pixel to be debugged.
  (could potentially be implicitly selected by "debug this vertex/pixel")
	- at this point, the UI installs a hook target
	- (future opt: already start shader patching, pipeline recompilation
	  at this point, async)
- work is submitted with hook:
	- when the target command is executed, instead use the patched pipeline.
	  (block for the first time it is submitted? idk might be ok.
	   But then need a caching mechanism. Hash by shader+breakpointLine or smth)
	- in the hook state, a buffer and struct layout + names is returned
	  or nullopt, when breakpoint was not hit.
	  (future: Could make this work for multiple breakpoints at once,
	   returning multiple such pairs)

Shader patching:
- Probably easiest just by hand, without a framework.
- Remember some header information
- Iterate over instructions
	- Remember the OpLine where the breakpoint is set.
	- Remember all OpVariables (later: named instructions? if anyone is using it)
	  and their function owner?
		- ideally we built the CFG here and check which OpVariables stores
		  came before the breakpoint line. But that is for later, just
		  consider every variable in the function scope now I think.
		- what about callstacks? for now, do not capture anything I guess.
		  Later on: before any function call of the breakpoint function,
		  write additional data (at least opLine, possibly also local state?).
		  Repeat recursively for those functions, too.
			- hm just opline should be enough. If that call is selected in UI,
			  just switch breakpoint to that position then.
- Then, we know all variables at the breakpoint line.
	- build a buffer layout: just a linear list of all the (local?) variables
	  known and accessible at the breakpoint position. Also capture global
	  state, e.g. stage inputs?

- allocate a device_address buffer with the needed size (known via type layout)
	- that buffer is hard-connected then to the patched module,
	  same lifetime
- Patch shader
	- Insert constant global value with the device address
	- After breakpoint OpLine, insert ops to copy all variables into
	  that buffer device address at their respective offsets
- create the shader module, compile the pipeline, if needed
	- what about shader objects?
- draw/dispatch/traceRays with new module/pipeline
	- afterwards, copy from the associated buffer to hook-specific, host_local
	  one
	- we have to make sure that not two queues can use the shader-buffer at the
	  same time. But we disallow this shader debugging in local captures ->
	  it can only be one queue. So shouldn't be an issue.

```
// declare type of struct to save holding all variables to save
%DstStruct = OpTypeStruct ... /* the variable types to store */
// declare physicalStorageBuffer pointer type for struct
%PhysicalBufferPointerDst = OpTypePointer PhysicalStorageBuffer %DstStruct

// Create struct of variables to save
%mem1 = OpLoad %var1
...
%structSrc = OpCompositeConstruct %DstStruct %mem1 ...

// Create variable of type PhysicalBufferPointerDst with hard-coded buffer address
%bufAddress = OpConstant %PhysicalBufferPointerDst /* hard-coded address */
// Create access chain for storing to buffer struct
// TODO: not sure if int_0 is needed. Meh, types here look weird.
%structDst = OpAccessChain %PhysicalBufferPointerDst %bufAddress %int_0
  OpStore %structDst %structSrc

```

I dislike the hardcoded address a bit.
Maybe at least use the same (fixed size, idk, 64k) buffer for all patched
modules? can't be active at the same time.

---

WAIT, can we create a pointer with OpConstant? Not sure.

---

I guess for cpu-side representation of the struct, we can use buffmt.
It becomes just one vil::Type (and a LinearAllocator).
So, we have in CommandHookState:

```
struct CopiedShaderData {
	Type type; // always a struct
	OwnBuffer data;
};
```

Later on, can add additional metadata (e.g. callstack) to dst data buffer.
We build this Type during shader patching.
For OpLine candidates for the breakpoint, remember the start of their
function. When we have our selected candidate, evaluate all OpVariable
values in that function (that came before the OpLine instruction itself).
