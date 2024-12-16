# Todo

v0.3:
- fix urgent bug list
- image viewer improvements
- vertex viewer improvements
- improve README, add more gifs/pics

urgent, bugs:
- [ ] fix standalone window version
	- [ ] broken by commit that removed window creation from device init, issue 21
- [ ] fix image viewer layout
- [ ] viewing texture in command viewer: show size of view (i.e. active mip level),
      not the texture itself. Can be confusing otherwise
	- [ ] maybe show full image size on hover?
	- [ ] also fix mip/layer selector that sometimes automatically resets itself
		  (seen with slice 3D selector e.g. npt surfel lookup tex)
- [ ] finish submissions in order, see CommandHookSubmission::finish
      comment for accelStruct captures
- [ ] immediately free HookedRecords that are not to be re-used in finish.

- [ ] test more on laptop, intel gpu
	- [ ] seems like we do some nasty stuff in the histogram shaders,
		  get gpu timeouts (try e.g. with curlnoise.ktx, zoom out on histogram)
	- [ ] find out why mesa drivers don't like transform feedback.
	      likely an issue on our side? but what?

- [ ] fix swapchain present for async present
      when present happens on different queue than our gui drawing, we currently
	  don't properly sync
	- real problem in doom eternal with 'present from async compute'
	- investigate if the current approach even works with same-queue:
	  we submit the wait semaphores to the queue just-like-that. Does
	  QueuePresent respect submission order though?
	- check if fixed now with single-swapchain-present restriction.
- [ ] figure our why overlay on doom is broken
      {seems to be due to async compute present}
	- [ ] also fix the semaphore crash. Run with debug output enabled.
- [ ] figure out tracy crashes with doom eternal :(
- [ ] fix with DLSS (doom)

- [ ] convert WM_INPUT mousePos in win32.cpp to AddMousePosEvent.
      just track internally?

- [ ] more xfb testing. Seems to currently cause issues with some mesa
      linux drivers as well (see laptop intel and amd card)

- [ ] histogram minMax auto-range not correctly working on all channels?
      just ignored the highs of the green-channel in a metallic/rough texture

- [ ] fix buffmt for storageBuffer array (crashes atm, does not expect array on that level)
      test with iro, shadowCull
	- [ ] a lot of descriptor code was probably never really tested for array bindings.
	      Make sure everything works.

- [ ] fix syncval hazards in gui (try out commands, e.g. transfer UpdateBuffer)
- [ ] windows performance is *severely* bottlenecked by system allocations from LinearAllocator.
      Increased it temporarily but should probably just roll own block sub-allocator
	  (try to test with RDR2 again)

new, workstack:
- [ ] add mode where hooking is disabled. Commands can still be inspected
	but, like, just statically.
- [ ] add isStateCmd(const Command&) and remove remaining command dynamic casts
- [ ] handle imgui cursor-to-be-shown and clipboard
	- [ ] make sure to pass it via interface
	- [ ] with hooked overlay, we have to implement it ourselves
- [ ] when viewing resources aliasing others in memory in the resource viewer,
      we have to make sure that their content wasn't made undefined.
	  Vulkan says it's not allowed to use such resources.
	  Note that they are usually NOT in the invalid image layout, just the last
	  one they were used in.
- [ ] VK_KHR_ray_tracing_maintenance1 for vkCmdTraceRaysIndirect2KHR
- [ ] VK_EXT_depth_clip_enable
- [ ] VK_EXT_sample_locations
- [ ] VK_EXT_line_rasterization
- [ ] VK_AMD_buffer_marker, VK_NV_device_diagnostic_checkpoints
	- [ ] test if vil works with the crash-report layer
- [ ] revisit inline button/refButton. Full button looked better at *some* places
- [ ] resource viewer: maybe use mechanism that just stores the
      new selection (e.g. from refButton) for the next frame instead of
	  selecting immediately? Need quite some checks/returns in there.
	  Error-prone atm!
- [ ] make use of proper sync tracking
	- [ ] fix added sync: only sync with active pending submission?
	      not sure how to properly do this
	- [x] proper setting of image's pending layout
	- [ ] introduce first cow-like concept, just tracking when resources get
	      modified
- [ ] improve tabs ui:
	- [x] try out top-rounded corners {looks whack},
	- [ ] play around further with alpha,
	- [x] signal somehow if window is focused or not
		  Either make tab-background change color on being focused,
		  make it more transparent or something or maybe use a thin window border?
		  {think window border looks good imo}
- [ ] cleanup: move out device_default stuff to gfr fork?

- [ ] (low prio, ui) sparse: for images, allow to show popup with bound
      memory on hover in imageViewer. Also bond-to-memory overlay
- [ ] (hight prio) sparse: come up with some idea on how to support
      partial un/re-binds. Applications might actually use this.
- [ ] properly show sparse bindings in frame UI
	- [ ] allow to inspect each bind/unbind
	      should be possible to share code with resource mem state viz
	- [ ] how to stabilize/make it possible to inspect sparse bind submissions?
	      they are only there for a single frame.
		  Implement something like "freeze on QueueBindSparse"?
- [ ] sparse bindings: make the resource mem ui a table instead,
      looks a bit messy at the moment.
- [ ] rework buffmt with proper array types (and multdim arrays)
      allow to store spirv u32 id per Type.
	  	- [ ] related to storageBuffer bug?
- [ ] cbGui: freezing state will currently also freeze the shown commands,
      might not be expected/desired. Reworking this is hard:
	  when freezing state we don't execute any hooks and therefore would need
	  new mechanism to correctly match new records.
	  Fix this when doing the next match/cbGui update iteration (related: next
	  command group impl iteration)
	  	- I guess just kicking out the '!selector_.freezeState' condition
		  from the force-update logic should be enough? Just make
		  sure to not show the "not found in X frames" ImGuiText then

- [ ] single-commandRecord viewing is buggy
	- [ ] useful feature, we want to support it.
- [ ] (low prio, debug) add global mutex priorities and add debug asserts
- [ ] improve gui layout, too much wasted space in buffer viewer rn.
- [ ] we often truncate buffers when copying them in record hook.
      Should *always* show some note in the UI, might be extremely confusing
	  otherwise
- [ ] cleanup: move render buffer code from Window/Overlay to gui.
      code duplication in Window/Overlay and it probably makes more sense
	  in Gui anyways?!
- [ ] {later} support for multiple swapchains: store the selected swapchain
      in CommandBufferGui/CommandSelection/CommandHook instead of using
	  dev.swapchain. Rename dev.swapchain into dev.lastCreatedSwapchain and
	  really only use it for the respective API call
	- [ ] regarding overlays: lazily create the gui for the first swapchain/Platform
	      that opens it? Allow switching later on though.
- [ ] add undefined behavior analyzer in shader debuggger.
      just need to enable spvm feature
- [ ] {low prio} update pegtl
- [ ] improve timing queries
	- [x] allow timing for whole command buffers (begin to end)
	- [ ] allow timing for whole submissions (begin to end)
	- [ ] allow (via button) to show a histogram for a specific timing query
		- [ ] create a small TimingQuery widget
	- [ ] stabilize it? (functionality for the TimingQuery widget).
	      Like, only update a couple of times per second?
		  {NOTE, we have UpdateTick now, not sure if needing a separate
		   mechanism just for timing queries}
- [ ] optimization(important): for images captured in commandHook, we might be able to use
      that image when drawing the gui even though the associated submission
	  hasn't finished yet (chained via semaphore).
	  Reducing latency, effectively having 0 frames
	  latency between rendered frame and debug gui anymore. Investigate.
	  (For buffers this isn't possible, we need the cpu processing for
	  formatting & text rendering)
	- [ ] maybe have a second vector<CommandHook> with pending submissions?
	      and if the user of the submissions is ok with pending resources,
		  it can use them?
- [ ] optimization: when hooked submission of a record with one_time_submit
      flag has finished, destroy the HookRecord
- [ ] investigate callstack performance for big applications.
      For windows, the current backward implementation uses StackWalk64, which
	  is super slow. Try CaptureStackBackTrace instead
- [ ] blur.comp: correctly blur in linear space, not srgb
      looks different currently depending on whether swapchain is srgb or not
- [ ] figure out to handle copyChain in a general way. Sometimes we need
      deep copies, sometimes we need to unwrap additional handles inside
	  the copy. Both of it is currently not done.
	  Or maybe remove the copyChain logic completely? Not sure if trying
	  to support simple extensions out of the box even makes sense.
- [ ] {later} honor maxTexelBufferElements. Fall back to raw vec4f copy
	- most impls have valid limits though. Had to fix the mock icd tho :/
- [ ] {later} implement blit imageToBuffer copy

ImageViewer-next:
- [ ] implement basic filters in image viewer, like inf/nan overlay
- [ ] {high prio} Find a way to handle viewport/scissor more elegantly,
      making sure they don't pop out of the window, EVER.
	  Probably need to store different values for viewport and scissor size.
	  Viewport depending on desired size, scissor depending on available
	  size (e.g. needed when hidden via scrolling)
- [ ] displaying high-res images in small viewer gives bad artefacts
      since we don't use mips. Could generate mips on our own (this requires
	  just copying the currently vieweed mip and then generating our own mips)
- [ ] there should probably just be one ImageViewer object; owned by Gui directly.
      it's then used wherever an image is shown.
	  Then, the ImageViewer could create and own its pipes, simplifying
	  all that gui setup.
- [ ] Improve/fix layout of image viewer
	- [ ] Keep the different split types tho
	- [ ] allow histogram to have more height?
- [ ] Put image information in metaInfo tab or add popup over image area
- [ ] allow viewing an image fullscreen instead of the overlay
      possibly still forwarding input to the application
	  (useful for debug-visualizing render targets, most engines have this)
- [ ] See todos in ImageViewer::drawMetaInfo

On vertices and where to capture them:
- [ ] fix capturing. Only capture the portions of the buffers that are used.
	- [ ] fix vertexViewer bug for indexed drawing where we truncate the
	      captured vertex buffers but can't really know/handle that
		  while drawing the captured vertices
	- [ ] show in UI if we truncate anything
- [ ] vertex viewer: show pages
- [ ] vertex viewer: make rows selectable, show vertex in 3D view
- [ ] figure out why copying attachments/descriptors shows weird/incorrect
      output in the dota intro screen sometimes. Sync problem? Matching problem?
	  {might be fixed now, with proper splitrp deps}
- [ ] fix vertex viewer for POINT toplogy (need to write gl_PointSize in vert shader)
- [ ] allow to visualize primitives (connected to vertices)
- [ ] allow to visualize non-builtin attributes somewhow
	  maybe also allow to manually pick an attribute to use as position
	  for the input?
- [ ] (later) improve camera, probably better to not lock rotation or use arcball controls
- [ ] (later) figure out when to flip y and when not.
      {Not as relevant anymore now that we render the frustum. Could still
	   be useful option. I guesse it's nothing we can just figure out}
- [ ] (later) better perspective heurstic. Also detect near, far (and use for frustum
      draw). Don't execute the heurstic every frame, only when something changes?
	  (Technically, data potentially changes every frame but the assumption
	  that drawn data doesn't suddenly change projection type seems safe).
- [ ] (later) really attempt to display non-float formats in 3D vertex viewer?
- [ ] (low prio) xfb: check whether a used output format is supported as input
	- [ ] also handle matrices somehow
- [ ] (later) support showing all draws from a render pass?

Freeze/selection changes:
- [ ] wayland VIL_CREATE_WINDOW freezes of window:
      when moving the application window to another workspace, the
	  driver just indefinitely blocks in QueuePresent. The problem is,
	  we have the queue mutex locked while that happens.
	  And so our window thread just starves at some point, trying
	  to acquire that mutex :/
	  I have no idea how to solve that at the moment.
	  We just can't submit while that is happening.
	  Also see https://github.com/KhronosGroup/Vulkan-Docs/issues/1158
	  QueuePresent also has no timeout parameter we could pass
	  when forwarding it :(
	  	- will probably not happen when applications use mailbox mode
		  Should at least document it somewhere.
		  Might also happen when the window is minimized on some platforms?

shader debugger:
- [x] cleanup/fix freezing as described in node 2235
- [x] implement breakpoints
	- [x] issue: we currently check for equality for breakpoints.
	      breakpoints for lines that don't have code associated with them
		  in spirv won't trigger. Need to do a more proper check
	- [ ] clean up breakpoint handling
- [ ] factor out retrieving descriptor from varID+indicies as used
      in load/store/arrayLength callbacks. Code duplication atm.
- [ ] set spec constants for shader module in gui shader debugger.
      Test with shader from tkn/iro
- [ ] detect unsupported features/capabilities (such as subgroup ops)
      and display "unsupported feature: X" error message in gui
- [ ] test, fix, cleanup handling of multiple files. Broken
      with breakpoints and their visualization.
- [ ] add UI for selecting workgroup/invocation
- [x] return workgroup size from shader in loadBuiltin
	- [ ] TODO: test
- [x] add support for loading push constants
	- [ ] TODO: test. Can we write unit tests for this? Should be possible
- [ ] support vertex shaders
	- [ ] correctly wire up the vertex input. And add ui for selecting
	      instance/vertex id to debug.
- [ ] support fragment shaders
	- [ ] figure out how to wire up input. Sketch: allow to select the
	      pixel, then select the primitive in the current draw call
		  covering the pixel (if more than one; or always use the last
		  one if it makes sense via vulkan drawing order guarantees?).
		  We then interpolate the input we got from xfb and use that
		  as input to the fragment shader.
	- geometry and tesselation shaders can remain unsupported for now.
- [ ] support ray tracing pipelines
	- [ ] as with compute shaders, we want to select the dispatch index
	- [ ] add support in spvm. Not sure about callback interface, probably
	      just pass the parameters from TraceRay to the application callback
		  and then let the application return the hit?
		  Hm, no, it's probably better to let the application then handle
		  everything (i.e. invoking all the required intersection/hit/miss shaders)
		  and just return/modify the ray payload, right?
	- [ ] aside from debugging the shaders (and the acceleration structure
		  hitting process), allow to visualize the rays (we can probably
		  just do a very small number of rays) in the acceleration
		  structure.
	- [ ] if we are serious about it, we need to really build our own
	      host-side acceleration structures
- [ ] add proper stack trace (ui tab)
	- [ ] allow to jump to positions in stack trace
- [ ] allow to view all sources in ui
- [ ] figure out where to put the "Debug shader" buttons
- [ ] add support for stores. And make sure reading variables later
      on return the correct values. Might need changes in spvm, the
	  was_loaded optimization is incorrect in that case.
	  Maybe set was_loaded to false when the OpVariable was stored to?
- [ ] clean up implementation. How we gather/display variables
      Make sure the variable display tree nodes can opened while the
	  state is being recreated (with "refresh" set. Should probably just
	  use the name, not its pointer as well. Figure out why we did
	  it for buffmt. arrays?)

spvm:
- [x] Add OpSpecConstant* support
- [x] Add OpArrayLength support
- [ ] merge back changes upstream
	- [ ] asserts
	- [x] improved image sampling
	- [ ] external variable load/store via callback
	- [ ] other missing opcodes implemented now

glslang (slightly unrelated):
- [ ] add PR that disables naming of "param" OpVariables
- [ ] add PR that adds flag to use OpLine with columns for expressions

docs
- [ ] write small wiki documentation post on how to use API
	- [ ] could explain why it's needed in the first place. Maybe someone
	      comes up with a clever idea for the hooked-input-feedback problem?
	- [x] write something on instance extensions for compute-only applications
	      see: https://github.com/KhronosGroup/Vulkan-Loader/issues/51
		  {see docs/compute-only.md}

window/overlay
- [ ] make the key to toggle/focus overlay configurable
	- [ ] we should probably think of a general configuration mechanism by now
	      just use qwe and store a file somewhere?
	- [ ] also remove the hardcoded check in vilOverlayKeyEvent I guess.
	      maybe entirely leave it up to the application to show/hide it?
- [ ] dota: figure out why hooked overlay does not work anymore when in game
      probably something to do with the way we grab input over x11
- [ ] fix/implement input pass-through when the hooked overlay does not have
      focus for win32.
- [x] improve windows overlay hooking. Experiment with mouse hooks blocking
      input.
	- [ ] implement further messages, keyboard, mouse wheel
	- [ ] clean the implementation up
	- [ ] when not showing own cursor, just use GetCursorPos over
	      raw input. Causes problems at the moment
- [ ] improve window creation: try to match up used swa backend with enabled
	  vulkan extensions. Could extend swa for it, if useful
	  (e.g. swa_display_create_for_vk_extensions)
	- [ ] possibly fall back to xlib surface creation in swa
- [ ] add a window icon for the separate window
      guess we need to implement it in swa for win32 first
- [ ] {low prio, later} fix overlay for wayland. try xdg popup?

performance/profiling:
- [ ] add 'hook' fastpaths that don't do this whole matching thing
	  when we e.g. know it's a different queue (or when we already had
	  a pretty perfect match?)

- [ ] CommandRecord::doEnd is expensive (and has a way-too-long CS) improve that
	- [ ] ~CommandRecord is expensive (and it's sometimes called multiple times
	       from one doEnd/doReset?? figure out how)
		   This is expensive due to (1) refRecords unregistering and
		   (2, not sure, wild guess) due to HookRecord destruction?
		   We should be able to move HookRecord destruction out of
		   the critical section, if that really is an issue.
	- otoh it doesn't seem to be anything in particular, most CSs
	      (except ~CommandRecord) are short. It's just that it's called
		  at lot in different threads -> sync overhead.
		  Can we maybe get it completely lockless (or only dependent
		  on local per-cb lock?)
		  Maybe it's a HUGE list of pipeline layouts being freed?
		  Maybe try using a hashSet instead?
		  maybe it's a lot of secret destructors being called?
		  That in turn call notifyDestruction of the device?
- [ ] make sure it's unlikely we have additional DescriptorSetState references
	  on vkFreeDescriptorSets (with normal api use and no gui)
- [ ] don't allocate memory per-resource. Especially for CommandHookState.
	  Instead, allocate large blocks (we need hostVisible for buffers
	  and deviceLocal for images) and then suballocate from that.
	  Consider just using vk_alloc, not sure if good idea though as we might
	  have some slight but very special requirements.
- [ ] figure out a better way to retrieve xfb data (and data in general, same
      problem for huge structured buffers). For huge draw commands
      (especially multi-draw where the whole scene is rendered), we would
	  need giant buffers and the performance impact is huge
	  	- [ ] also figure out better xfb buffer allocation strategy,
		      just always allocating 32MB buffers is... not good.
		- [ ] solution: implement draw call splitting as in docs/own/cow.md.
		      We have to take care to always preserve all IDs passed to
			  the shader (gl_DrawIndex, gl_VertexIndex etc)
- [ ] profile our formatted data reading, might be a bottleneck worth
	  optimizing. VertexViewer.Table zone had > 10ms (even with just 100
	  vertices). Find the culprit!
	- [ ] In VertexViewer: use imgui list clipping! perfect and easy to use here
- [ ] (high prio) holding the device mutex while submitting is really bad, see queue.cpp.
      We only need it for gui sync I think, we might be able to use
	  a separate gui/sync mutex for that. Basically a mutex that (when locked)
	  makes sure our tracked state (e.g. dev.pending) really includes everything
	  that has been submitted so far. That mutex would have to be
	  locked before locking the device mutex, never the other way around.
- [ ] on windows, freeBlocks (after ~CommandRecord) can be a massive bottleneck
      (seen on systems that were running low on memory at the time).
	  We should not allocate/free blocks per CommandRecord but share them
	  via the CommandPool (if possible, it's kinda tricky due to CommandRecord lifetime).
	  Just re-add what we previously had. But this will need changes to CommandRecord::invalidated
	  and some re-thinking in general on how to allocate *after* recording is finished.
	  Might have to merge this with the refRecords/invalidated rework.
- [ ] {low prio now} can we make per-cb-mutexs a thing?
      major bottleneck for applications that have hundreds of small command
	  buffers. There are reasons it's not possible at the moment though,
	  the cb-handle connections for instance.
	  	- The main issue was an allocation while locking. Fixed now.
		  per-cb-mutex shouldn't be important as long as we keep the critical
		  sections really small, without any blocking

gui stuff
- [ ] (high prio) cb/command viewer: when viewing a batch from a swapchain,
      show the semaphores/fences with from a vkQueueSubmit.
	  When selecting the vkQueueSubmit just show an overview.
	  	- Currently hard to do, the fences/semaphores might have been destroyed.
		  Not sure how to easily track this.
		-> submission tracking rework
- [ ] imgui styling. Compare with other imgui applications.
      See imgui style examples in imgui releases
	- [x] use custom font
	- [ ] better markup in ui. E.g. color/somehow mark "Waiting for submission"
	      or error messages in command viewer.
	- [ ] (low prio) also use icons where useful (via icon font, like e.g. tracy does)
	- [ ] some of the high-information widgets (barrier command, rp, pipe viewers)
	      are really overwhelming and hard to read at the moment.
		  Can be improved to grasp information for intuitively
	- [x] add tooltips where it makes sense
	- [x] go at least a bit away from the default imgui style
	      The grey is hella ugly, make black instead.
		  Use custom accent color. Configurable?
	- [x] Figure out transparency. Make it a setting?
	- [ ] lots of places where we should replace column usage with tables
	- [x] fix stupid looking duplicate header-seperator for commands in
	      command viewer (command viewer header UI is a mess anyways)

other
- [ ] testing: at least make sure it does not crash for features we don't
	  implement yet
- [ ] (high prio) better pipeline/shader module display in resource viewer
      It's currently completely broken due to spirv-reflect removal
	- [ ] especially inputs/outputs of vertex shaders (shows weird predefined spirv inputs/outputs)
	- [ ] also make sure to handle in/out structs correctly. Follow SpvReflectInterfaceVariable::members
	- [ ] maybe display each stage (the shader and associated information) as its own tab
- [ ] when viewing live command submissions, clicking on resource buttons
	  that change every frame/frequently (e.g. the backbuffer framebuffer)
	  does not work. Wanting to goto "just any of those" is a valid usecase IMO,
	  we could fix it by not imgui-pushing the resource ID before showing the button.
	  	Instead just push the semantic that resource has in the given inspector
		to make sure buttons are unique.
- [ ] improve resource viewer
	- [ ] tables instead of columns (columns sometimes don't align)
	- [ ] references to other handles are so ugly at the moment
	- [ ] for many handles the page is not implemented yet or looks empty
- [ ] decide whether to enable full-sync by default or not.
      Explain somewhere (in gui?) why/when it's needed. Already described
	  in design.md from my pov
- [ ] implement "jump to selected" in commandRecord viewer, immediately
      opening all needed sections and centering the selected command.
	  Maybe give some special visual feedback when selected command can't
	  be found in current records?
- [ ] rename VIL_HOOK_OVERLAY to just VIL_OVERLAY?
      and VIL_CREATE_WINDOW to VIL_WINDOW?
- [ ] clean up logging system, all that ugly setup stuff in layer.cpp
	- [ ] also: intercept debug callback? can currently cause problems
	      e.g. when the application controlled debug callback is called
		  from *our* internal thread (which it might not be prepared for).
		  In interception, could check whether it involves one of our
		  handles or is called from our window thread.
	- [ ] when intercepting dlg, at least forward to old handler...
	- [ ] show failed asserts and potential errors in imgui UI?
	      probably best to do this in addition to command line
	- [ ] log assertions to debug console in visual studio via OutputDebugString
	      when we detect that there is an attached debugger.
	      Somehow signal they are coming from us though, use a VIL prefix or smth.
		  Stop allocating a console.
- [ ] figure out "instance_extensions" in the layer json.
      Do we have to implement all functions? e.g. the CreateDebugUtilsMessengerEXT as well?
- [ ] more useful names for handles (e.g. some basic information for images)
	- [ ] also: atm we always prepend the resource type leading to something
	      like "Buffer terrainBuffer". Add a parameter to the function whether
		  this should be done, it some contexts (e.g. CmdCopyBuffer,
		  resource viewer only viewing buffers) its very redundant.
		  In most cases, it's redundant, only useful for some buttons (but
		  even then we likely should rather have `Image: |terrainHeightmap|`.
- [ ] (low prio) improve imgui event handles, make sure inputs aren't lost when fps are low.
      see e.g. https://gist.github.com/ocornut/8417344f3506790304742b07887adf9f
- [ ] (low prio) show enabled vulkan11, vulkan12 features in gui as well
- [ ] (low prio) when neither VIL_HOOK_OVERLAY nor VIL_CREATE_WINDOW is set, should

local captures:
- [ ] {feature, useful} support regular hooks on
      local-capture-hooked records. Not exactly sure how this would work.
	  On a similar note, support hooking multiple commands in a single record
	  (most general case: multiple local hooks, multiple regular captures)
- [ ] {feature, later} add flag specifying to capture the frame context
      i.e. when showing it, show the whole frame.
	  Just store the submissions in the LocalCapture completed hook.

---

Possibly for later, new features/ideas:
matching:
- [ ] (low prio, later) implement 'match' for missing commands, e.g.
      for queryPool/event/dynamic state commands
- [ ] (for later) fix/improve command matching for sync/bind commands
	- [ ] bind: match via next draw/dispatch that uses this bind
	- [ ] sync: match previous and next draw/dispatch and try to find
	      matching sync in between? or something like that

object wrapping:
- [ ] (low prio, not sure yet)
      only use the hash maps in Device when we are not using object
      wrapping. Otherwise a linked list is enough/better.
	  Could always use the linked list and the hash maps just optionally.
	  Maybe we can even spin up some lockfree linked list for this? We'd only
	  ever insert at one end, order is irrelevant.
	  NOTE: linked list isn't always enough, for descriptor-bound handles
	  (i.e. views) we sometimes need to check whether a given handle is
	  still valid. But that is kinda error-prone anyways due to reallocation
	  of the same address later on :/
	- [ ] Figure out how to correctly handle the maps in Device when using
		  wrapping. Many ugly situations atm, see e.g. the
		  hack in ~CommandPool and ~DescriptorPool.
		  And we don't really need maps in the first place, see below.
- [ ] (later, low prio) support wrapping for remaining handle types.
      we can wait with this until it proves to be a bottleneck somewhere
- [ ] (later, low prio) support wrapping for VkDevice, fix it everywhere

improved matching
- [ ] (low prio) for sync/bind commands: find surrounding actions commands - as
      outlined multiple times - and find them. Might cross block/record
      boundaries tho
	  {we have something almost as good now by first matching sections and
	   then only 'find'-ing locally. This approach could still give
	   some improvements tho}
- [ ] (low prio) for order-dependent blocks, don't just use 'find' but use local
      matching instead? could get too expensive tho


descriptor indexing extension:
- [ ] support partially_bound. See e.g. gui/command.cpp TODO where we
	  expect descriptors to be valid. Might also be a problem in CommandHook.
- [ ] Make sure we have update_after_bind in
      mind everywhere. We would at least have to lock the descriptorSetState mutex
	  when reading it in Gui/match to support this, might get a race otherwise.
	  Or, probably better: hold the per-ds mutex locked during the whole
	  update process, sync refCount using it. For CopyDescriptorSet,
	  we can use std::lock.
- [ ] (low prio) See the TODO in CommandHookRecord::copyDs to fix
      support for updateUnusedWhilePending.
- [ ] (for later) investigate whether our current approach really
      scales for descriptor sets with many thousand bindings

ext support:
- [ ] VK_KHR_fragment_shading_rate: need to consider the additional attachment,
      would also be nice to show in gui

gui:
- [ ] (low prio, later?) resource viewer: basic filtering by properties, e.g. allow to just
      show all images with 'width > 1920 && layers > 3' or something.
	  Could start with images for now and add for other handles as needed.
	  Definitely useful for images, when exploring the resource space

optimization:
- [ ] (low prio) add (extensive) time zone to basically every entry point so we
	  can quickly figure out why an application is slow
- [ ] (low prio) setting spec constants every time we access a spc::Compiler
      object is inefficient since most applications don't reuse a shaderMod
	  object in multiple pipelines and we mostly don't randomly access them.
	  Maybe store last-used specialization constant object and compare
	  whether they are the same?
- [ ] (low prio) during reflection, we often call get_shader_resources()
      which is expensive. Maybe cache result?

---

- [ ] proper descriptor resource management for gui. We currently statically allocate
	  a couple of descriptors (in Device) and pray it works
	  	- [ ] when push descriptors are available we probably wanna use them.
		      alternative code path should be straight forward
- [ ] support multiple possible layouts (including scalar!) in bufparser
- [ ] Properly init/resize accel struct buffers on build.
	  See ~commandHook.cpp:1671 (needsInit)
- [ ] implement "freeze on NaN/inf"
- [ ] (low prio but highly interesting) optionally capture callstacks for each command
      immediately jumping to the point the command was recorded sounds
	  useful. Could build in support for vim and visual studio I guess.
	  See https://github.com/bombela/backward-cpp/blob/master/backward.hpp
	- [ ] or maybe just use tracy's backtrace facilities?
- [ ] buffer viewer: allow to convert the spirv buffer representation into text form
	  and edit it before viewing buffer data e.g. in command viewer?
	  Could really be useful for packed data.
	- [ ] related: just allow to reference known types anywhere?
	      If any shader module contained a struct type named "GlobalUbo",
		  allow to just use it in any context?
		  Would be *really* useful for debugging but might require too much
		  tracking. Also problematic with different std buffer layouts
- [ ] buffer viewer: allow to show data as hexadecimal
- [ ] buffer viewer: allow to show data in a more compact manner, binary-viewer-like
- [ ] we need to copy acceleration structures/accelStructs in commandHook/
      on submission. But not really physically copy but track their state
	  at that point when they are viewed. Hard to do in case the submission
	  also writes into them.
- [ ] figure out how to handle device address in UI.
	  We probably want to link to the related resource. Just solve
	  this via std::map in Device with custom comparison that
	  checks whether an address is in-range? Think about whether
	  this works for memory aliasing.
- [ ] (low prio, only when problem) memory overhead from using spirv-cross may be too large.
      Create the reflection modules only on-demand then, shouldn't be
	  too expensive. And/or page them and/or the stored spirv data
	  itself out to disk.
- [ ] raytracing: proper support for pipeline libraries
- [ ] correctly query transform feedback limits and respect them, e.g. buffer size
- [ ] (later?) correctly track stuff from push descriptors. Need to add handles and stuff,
      make sure destruction is tracked correctly. Also show in gui.
	  See the commands in cb.cpp
- [ ] vil_api.h: Allow destroying created overlays
- [ ] improve DeviceMemory visualization. Also improve Memory overview tab
- [ ] SetHandleName should probably not always use the device hash map
      when the objects are wrapped. Currently causes issues since
	  we don't insert descriptor sets when wrapping, we need the custom hack there
- [ ] track ext dynamic state in graphics state
	- [ ] show extended dynamic state in gui
- [ ] the current handling when someone attempts to construct multiple guis
      is a bit messy. Move the old gui object? At least make sure we always
	  synchronize dev.gui, might have races atm.
	  See TODO in Gui::init
- [ ] could write patched shader spirv data into a pipeline cache I guess
      or maintain own shader cache directory with patched spirv
	  not sure if worth it at all though, the spirv patching is probably
	  not that expensive (and could even be further optimized, skip
	  the code sections).
- [ ] get it to run without significant (slight (like couple of percent) increase
	  of frame timings even with layer in release mode is ok) overhead.
	  Just tests with the usual suspects of games
- [ ] when available on hardware I can test it on: support using
      VK_EXT_device_memory_report to get insights into additional memory
	  usage; expose that information via gui.
- [ ] support debug utils labels across command buffer boundaries
	  we already have the information of popped and pushed lables per record
      NOTE: this isn't 100% correct at the moment though, e.g. when we end
	  a debug label manually to fix the hierachy, it's only ended to the "ignoreEnd"
	  counter but if that end is never called (to be ignored) the label would
	  effectively be pushed onto the queue stack which we currently don't
	  store anywhere. Not sure how to correctly do this though atm, depends
	  on how we use it later on (since the label isn't effectively on the
	  queue stack for our fixed hierachy...)
- [ ] cmdDrawIndirectCount: allow to view state (especially attachments
      but i guess could be useful for ds state as well) before/after each
	  individual draw command. Same for cmdDrawIndirect with multiple
	  draw counts. Could likely use the same mechanism to do the same for
	  instances
- [ ] functions that allocate CommandRecord-memory should not take
      a CommandBuffer& as first argument but rather something like
	  CommandRecordAllocator (or just CommandRecord), for clarity.
- [ ] improve frame graph layout in overview. Looks not nice atm
	- [ ] maybe try out implot lib
	- [ ] instead of limiting by number of frames maybe limit by time?
		  the 1000 last timings (as it is right now) is bad, not enough for high-fps applications
- [ ] implement clipboard, cursor style and other feature support for imgui
	- [ ] in window.cpp, for our external debug window
	- [ ] where useful (and really needed) incorporate it into the public API
- [ ] (low, later) add own normals to vertex viewer (either somehow on-the-fly on gpu or
      pre-calculate them on cpu) and add basic shading.
	  maybe we can reuse existing normals in some cases but i doubt it,
	  no idea for good heuristics
- [ ] when selecting a draw call, allow to color it in final render
	- would require us to modify shader/pipeline. lots of work.
	  Alternatively, we could simply render over it with a custom pipeline
- [ ] the gui code is currently rather messy. Think of a general concept
      on how to handle tabs, image draws and custom pipelines/draws inside imgui
- [ ] reading 64-bit int formats might have precision problems, see the format
      io in util.cpp
- [ ] Implement missing resource overview UIs
	- [ ] sync primitives (-> submission rework & display, after v1.1)
- [ ] in vkCreateInstance/vkCreateDevice, we could fail if an extension we don't support
      is being enabled. I remember renderdoc doing this, sounds like a good idea.
	- [ ] or an unexpectly high api version
		  (allow to disable that behavior via env variable tho, layer might work
		   with it)
	- [ ] overwrite pre-instance functions vkEnumerateInstanceExtensionProperties?
	      then also use the vkGetPhysicalDeviceProcAddr from the loader to
		  overwrite extension enumeration for the device.
		  And filter the supported extensions by the ones that we also support.
		  Evaluate whether this is right approach though. Renderdoc does it like
		  this. Alternatvely, we could simply fail on device/instance creation
		  as mentioned above (probably want to do both approaches, both disableable
		  (just found a new favorite word!) via env variable
- [ ] add feature to see all commands that use a certain handle.
      we already have the references, just need to add it to command viewer.
	  Just add a new command viewer mode that allows to cycle through them.
	- [ ] make sure to select (and scroll towards) the commands when selecting
	      them. Not exactly sure how to implement but that command should
		  even be shown in command list when its type is hidden (maybe
		  make it a bit transparent, "ghost-command")
- [ ] in some places we likely want to forward pNext chains by default, even
      if we don't know them. E.g. QueuePresent, render pass splitting?
	  Whatever gives the highest chance of success for unknown extensions.
	  (Could even try to toggle it via runtime option)
- [ ] improve buffer viewer {postponed to after v0.1}
	- [ ] NOTE: evaluate whether static buffer viewer makes much sense.
	      Maybe it's not too useful at the moment.
	- [ ] ability to infer layouts (simply use the last known one, link to last usage in cb) from
		- [ ] uniform and storage buffers (using spirv_inspect)
		- [ ] vertex buffer (using the pipeline layout)
		- [ ] index buffer
		- [ ] texel data from a buffer/image copy
	- [ ] ability to directly jump to it - in the contextually inferred layout - from a command?
	      (might be confusing, content might have changed since then)
	- [ ] move to own tab/panel? needed in multiple cases
- [ ] attempt to minimize level depth in cb viewer
	- [ ] when a parent has only one child, combine them in some cases?
	      mainly for Labels, they are currently not too helpful as they
		  make the hierachy just deeper...
	- [ ] allow per-app shortcuts to specific commands?
- [ ] look into imgui shortcuts to allow quick interaction.
      vim-like!
- [ ] support multiple subresources for transfer commands, images
	- [ ] pain in the ass regarding layout transitions as the range of
	      subresources does not have to be continuous (and we can't assume
		  all of them to be in same layout)
- [ ] important optimization, low hanging fruit:
      CopiedImage: don't ever use concurrent sharing mode.
	  We can explicitly transition the image when first copying it.
- [ ] we currently copy more levels/layers in commandHook than are shown
      in i/o inspector. Could just copy the currently shown subresource.
- [ ] write tests for some common functionality
	- [ ] format reading/writing; conversion
- [ ] clean up/unify usage of struct/class
	  struct for POD (with no public/private classifiers and member functions),
	  class otherwise I guess
- [ ] move external source into extra folder
- [ ] support timeline semaphores (submission rework/display)
- [ ] performance: when a resource is only read by us we don't have to make future
	  submissions that also only read it wait.
	- [ ] requires us to track (in CommandRecord::usedX i guess) whether
		  a resource might be written by cb
- [ ] support for multiple swapchains
	- [ ] in submission viewing, we assume there is just one atm
	- [ ] currently basically leaking memory (leaving all records alive)
	      when application has a swapchain it does not present to?
- [ ] include vkpp enumString generator here?
      allows easier updates, maintaining
- [x] support for buffer views (and other handles) in UI
	- [ ] use buffer view information to infer layout in buffer viewer?
	- [ ] support buffer views in our texture viewer (i.e. show their content)
- [ ] experiment with transparent overlay windows in which we render the
      overlay, to not be dependent on application refresh rate.
- [ ] allow to view submissions to a queue
- [ ] add support for ext mesh shaders
- {low prio} implement at least extensions that record to command buffer
  to allow hooking when they are used
	- [ ] device masks (core vulkan by now)
	      ugh, supporting this will be a MAJOR pain, especially gui rendering.
	- [ ] nv device diagnostic checkpoint
	- [ ] nv exclusive scissor
	- [ ] amd buffer marker
	- [ ] intel performance metrics
	- [ ] nv shading rate image
	- [ ] nv viewport scaling
	- [ ] transform feedback (not sure we want to support this at all?)
	      we probably should support it eventually, just disable our patching
		  for pipes/shaders that use it themselves. Vertex shader won't be available
		  in that case.
	- [ ] nv shading rate enums
- [ ] use new imgui tables api where useful
- [ ] add "save to ktx" feature on images? Personally, I'd consider this
      useful but this will likely scream LETS ABUSE PROPRIETARY IMAGES to some
	  evil creatures out there so not sure if this is a good idea.
	  Maybe just don't enable it in default build config?
	- [ ] pretty much same for writing out buffer contents to a file
	- [ ] could export full models from drawcalls via gltf
		  (without textures or at unassigned textures or maybe even
		   try to connect them to gltf properties via heuristics)
- [ ] support for compressed image formats
- [ ] optimize: suballocate CopiedBuffer
- [ ] optimize: reuse CopiedImage and CopiedBuffer
- [ ] support multiple imgui themes via settings
- [ ] in cb viewer: allow to set collapse mode, e.g. allow more linear layout?
      and other settings
- [ ] make gui more comfortable to use
	- [ ] add shortcuts for important features
	- [ ] investigate if we can make it vim-like navigatable keyboard-only
	- [ ] make "back" button work (and add one it gui itself?)
- [ ] investigate the vulkan layer providing the timeline semaphore feature.
      We could advise its usage when a native implementation is not available.
	  Eventually we could (either by requiring use of the layer when ext not
	  implemented by driver or by implementing it inside our layer) expect
	  timeline semaphores to be available, removing the legacy code path.
- [ ] fix warning 4458, see meson.build, we currently disable it.
- [ ] internal statistics tab
	- [ ] number of hooked command buffer records (alive records)
	- [ ] time spent in certain critical sections?
	- [ ] memory allocated in commandpools?
	- [ ] total number of handles or something, giving an estimate of memory consumption
- [ ] the way we currently split render passes does not work for resolve
	  attachments (except when they are in the last subpass) since
	  the resolve might be done multiple times, overwriting old results :(
	  In that case: either just don't allow command hooking (for now) or
	  just do the expensive solution: completely modify the render passes
	  and recreate all framebuffers and graphics pipelines ugh
- [ ] somehow display submissions and their dependencies as a general
      frame-graph like overview (could go down to renderpass-level, to
	  provide a full frame graph).
	- [ ] Maybe start with something that simply writes a dot file for a frame or two?
	      Getting this right interactively with imgui will be... hard
- [ ] register own debug messenger if possible?
- [ ] for command descriptions, take pNext chains into account.
	  they are rarely changed just like that (neither is their order I guess)
- [ ] track push constant range pipe layouts? correctly invalidate & disturb
      also track which range is bound for which stage.
- [ ] can we support viewing multisample images?
      either sample them directly in shader (requires a whole lotta new
	  shader permuatations, not sure if supported everywhere) or resolve
	  into temporary image first (lot of work as well)
- [ ] better installing
	- [ ] simple wix windows installer, just needs to install prebuilt layer,
	  	   json file and add the registry file. Should probably also install
	       api header tho
		   (maybe for later, >0.1.0?)
	- [ ] write AUR package (maybe for later, >0.1.0?)
- [ ] allow to display stuff (e.g. images) over swapchain, fullscreen, not just in overlay
- [x] memory budget overview {present but ugly as-is}
	- [ ] show how much memory was allocated overall, per-heap
	      make this visually appealing, easy-to-grasp. Maybe via pie-chart or something.
		  We can probably start using ImPlot.
		  Maybe allow to have a global pie chart (showing *all* memory) and
		  then per heap/per memory type flag (allowing us to easily visualize
		  e.g. the amount of on-device memory allocated/available).
	- [ ] Also allow to color memory depending on the type it is allocated for.
	- [ ] allow to visualize by allocation size, i.e. showing the big memory blockers
	      (but also allow showing smallest first, can be useful)
	- [x] visualize totally available memory as well, we can get that from
	      memory properties
	- [x] there are extensions for querying the real allocated/free memory
	      size. Use them!
- [ ] simulate device lost: Just make the layer return deviceLost from
      all (or a lot of) commands until the device is recreated.
	  Useful for testing!
- [ ] small buffer optimization for global hash tables (that are most
      commonly accessed). Maybe add extra fast path for single-device case?
	  (having something like Device* lastDevice in global scope, would
	  still need mutex but spare us hashing and lookup and extra cache miss)
- [ ] show histogram for query pool timings (for inserted ones, but could
      also do it for application query pool timings).
- [ ] add per-section and per-commandbuffer query pool timings
- [ ] directly show content for imageview? with correct format/mip/layer etc?
- [ ] (somewhat high prio tho) add support for waiting for command buffer
      recording to finish (with a timeout tho, in which case we simply display
	  that is currently being recorded (and that it takes long)), when viewing
	  it. Especially a big problem for display-window (compared to overlay)
	  when an application re-records in every frame.
	  	- [ ] could be done via conditional variable in command buffer
		      that is signaled on endCommandBuffer
- [ ] give visual/explicit feedback about commandBuffer change in UI?
	  maybe show time/frames since last re-record?
	  Show statistics, how often the cb is re-recorded?
- [ ] mode that allows to simply view all commands pushed to a queue?
- [ ] we might be able (with checks everywhere and no assumptions at all, basically)
      to support cases where extensions we don't know about/support are used.
	  (e.g. image is created via a new extension, we don't hook that call,
	  image is then used somewhere). Evaluate if it is doable. If so, we should
	  really try. Even if we don't catch all cases it will make this a lot
	  easier to maintain (and make it seem less of a buggy mess to users)
- [ ] support vulkan 1.1 (non-crash)
	- [x] bindmemory2
	- [x] support descriptor set update templates
	- [x] support vkCmdDispatchBase
	- [ ] support device mask stuff (non-crash)
		- [ ] allow to hook command buffers containing it
		- [ ] layer might break with this though, not sure if we can support it easily
			  for real multi-gpu. Investigate (not supporting it for now is
			  okay but document why).
	- [ ] support everything in UI
		- [ ] add sampler ycbcr conversion tracking
- [ ] support vulkan 1.2 (non-crash)
	- [ ] support everything in UI
	- [x] CmdDrawIndirectCount
	- [x] CmdDrawIndexedIndirectCount
	- [x] other new creation and commands
- [ ] interactive 3D cubemap viewer
- [ ] interactive 3D model viewer with as many information as possible
- [ ] event log showing all queue submits
	- [ ] optionally show resource creations/destructions there as well
- [ ] better pipeline state overview of inputs, stages, outputs
	- [ ] maybe via a graph?
- [ ] we might be able to properly hook input (without needing the public api)
	  by using a (movable?) child window for our overlay instead of directly
	  presenting to the swapchain.
- [ ] when rendering in own window: continue to dispatch display while
      waiting for application fence. This allows to track really long
	  submissions (e.g. for compute) without losing responsiveness.
	  (Just show something like "image/buffer is in use in the ui")
	  -> sync rework/semaphore chaining, i guess (but note how it's **not**
		 solved with semaphore chaining alone!)
- [ ] all this dynamic_cast'ing on Command's isn't good. There is a limited
      number of commands (and we never absolutely need something like
	  dynamic_cast<DrawCmdBase*> i guess?) so we could enum it away.
	  But otoh dynamic_cast and hierachy is probably better for maintainability.
	  But we should replace DrawCmdBase dynamic_casts with checks for
	  type() == CommandType::draw
- [ ] (low prio) can we support android?
- [ ] (low prio, non-problem) when using timeline semaphores, check for wrap-around?
      would be undefined behavior. But I can't imagine a case in which
	  we reuse a semaphore 2^64 times tbh. An application would have to
	  run millions of years with >1000 submissions per second.
	  Probably more error prone to check for this in the first place.
- [ ] (low prio), optimization: in `Draw`, we don't need presentSemaphore when
	  we have timeline semaphores, can simply use futureSemaphore for
	  present as well
