# Todo

v0.1, goal: end of january 2021 (edit may 2021: lmao) 
- overlay improvements (especially win32 but also x11; leave wl out)
- testing & stability improvements
- docs improvements (mainly: add introduction post explaining stuff)
- gui improvement: remove flickering data and stuff, allow to get
  texel values in command viewer

new, workstack:
- [ ] implement image histograms (probably best like in PIX)
      See minmax.comp and histogram.comp
- [ ] implement basic filters in image viewer, like inf/nan overlay
- [ ] optimization(important): for images captured in commandHook, we might be able to use
      that image when drawing the gui even though the associated submission
	  hasn't finished yet (chained via semaphore).
	  Reducing latency, effectively having 0 frames
	  latency between rendered frame and debug gui anymore. Investigate.
	  (For buffers this isn't possible, we need the cpu processing for
	  formatting & text rendering)

urgent, bugs:
- [ ] figure out tracy issues. On windows, it causes problems with a lot
      of applications that I can't explain :( Some of the problems
	  have been caused by a tracy update, it used to work!
- [ ] figure out transform_feedback crashes in doom eternal
- [ ] viewing texture in command viewer: show size of view (i.e. active mip level),
      not the texture itself. Can be confusing otherwise
	- [ ] maybe show full image size on hover?
- [ ] vertex viewer: show pages
- [ ] vertex viewer: make rows selectable, show vertex in 3D view
- [ ] figure out why copying attachments/descriptors shows weird/incorrect 
      output in the dota intro screen sometimes. Sync problem? Matching problem?
	  {might be fixed now, with proper splitrp deps}
- [ ] figure out why spirv-cross is sometimes providing these weird names
	  (e.g. for buffers; something like _170_2344) instead of simply having 
	  an empty alias string
- [ ] toupper bug when searching for resource
- [ ] fix vertex viewer for POINT toplogy (need to write gl_PointSize in vert shader)

spvm:
- [ ] Add OpSpecConstant* support
- [ ] Add OpArrayLength support
- [ ] Avoid copies for setting buffer data.
      Maybe just add callback when a buffer is accessed that can return the data?
	  I guess the only ways for access is OpLoad, optionally via OpAccessChain.
	  Something like `spvm_member_t get_data(spvm_result_t var, size_t index_count, unsigned* indices)`
	  Alternative: a more specific interface only for the most important case, runtime arrays.
	  (But honestly, shaders could also just declare huge static arrays so we probably
	  want the general support).
- [ ] add callback for getting image data instead of requiring the whole image
      to be present

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
- [ ] improve windows overlay hooking. Experiment with mouse hooks blocking
      input.
	- [ ] implement further messages, keyboard, mouse wheel
	- [ ] clean the implementation up
	- [ ] when not showing own cursor, just use GetCursorPos over
	      raw input. Causes problems at the moment
- [ ] improve window creation: try to match up used swa backend with enabled
	  vulkan extensions. Could extend swa for it, if useful
	  (e.g. swa_display_create_for_vk_extensions)
	- [ ] possibly fall back to xlib surface creation in swa
	- [ ] at least make sure it does not crash for features we don't
	      implement yet (such as sparse binding)
		   (could for instance test what happens when memory field of a buffer/image
		   is not set).
- [ ] add a window icon for the separate window
      guess we need to implement it in swa for win32 first
- [ ] {low prio, later} fix overlay for wayland. Use xdg popup

performance/profiling:
- [ ] don't hook every cb with matching command. At least do a rough check
      on other commands/record structure. Otherwise (e.g. when just selecting
	  a top-level barrier command) we very quickly might get >10 hooks per
	  frame.
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
- [ ] fix the optimization we have in ds.cpp where we don't store DescriptorSets into the
      hash maps when object wrapping is active (which currently causes the gui to not
	  show any descriptorSets in the resource viewer)
- [ ] also don't always copy (ref) DescriptorSetState on submission.
      Leads to lot of DescriptorSetState copies, destructors.
	  We only need to do it when currently in swapchain mode *and* inside
	  the cb tab. (maybe we can even get around always doing it in that
	  case but that's not too important).
	  For non-update-after-bind (& update_unused_while_pending) descriptors,
	  we could copy the state on BindDescriptorSet time? not sure that's
	  better though.
	- [ ] also don't pass *all* the descriptorSet states around (hook submission,
	      cbGui, CommandViewer etc). We only need the state of the hooked command, 
		  should be easy to determine.
	- [ ] See the commented-out condition in submit.cpp


object wrapping:
- [ ] only use the hash maps in Device when we are not using object 
      wrapping. Otherwise a linked list is enough/better.
	  Could always use the linked list and the hash maps just optionally.
	  Maybe we can even spin up some lockfree linked list for this? We'd only
	  ever insert at one end, order is irrelevant.
	- [ ] Figure out how to correctly handle the maps in Device when using 
		  wrapping. Many ugly situations atm, see e.g. the
		  hack in ~CommandPool and ~DescriptorPool.
		  And we don't really need maps in the first place, see below.
- [ ] (later, low prio) support wrapping for remaining handle types.
      we can wait with this until it proves to be a bottleneck somewhere
- [ ] (later, low prio) support wrapping for VkDevice, fix it everywhere

gui stuff
- [ ] (high prio) cb/command viewer: when viewing a batch from a swapchain,
      show the semaphores/fences with from a vkQueueSubmit.
	  When selecting the vkQueueSubmit just show an overview.
- [ ] (high prio) figure out general approach to fix flickering data, especially
      in command viewer (but also e.g. on image hover in resource viewer)
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
- [ ] can we link C++ statically? Might fix the dota
	  std::regex bug maybe it was something with the version of libstdc++?
- [ ] (high prio) better pipeline/shader module display in resource viewer
      It's currently completely broken due to spirv-reflect removal
	- [ ] especially inputs/outputs of vertex shaders (shows weird predefined spirv inputs/outputs)
	- [ ] also make sure to handle in/out structs correctly. Follow SpvReflectInterfaceVariable::members
	- [ ] maybe display each stage (the shader and associated information) as its own tab
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
- [ ] when viewing live command submissions, clicking on resource buttons
	  that change every frame/frequently (e.g. the backbuffer framebuffer)
	  does not work. Wanting to goto "just any of those" is a valid usecase IMO,
	  we could fix it by not imgui-pushing the resource ID before showing the button.
- [ ] (low prio) improve imgui event handles, make sure inputs aren't lost when fps are low.
      see e.g. https://gist.github.com/ocornut/8417344f3506790304742b07887adf9f
- [ ] (low prio) show enabled vulkan11, vulkan12 features in gui as well
- [ ] (low prio) when neither VIL_HOOK_OVERLAY nor VIL_CREATE_WINDOW is set, should

---

Possibly for later, new features/ideas:
matching:
- [ ] (low prio, later) implement 'match' for missing commands, e.g.
      for queryPool/event/dynamic state commands
- [ ] (for later) fix/improve command matching for sync/bind commands
	- [ ] bind: match via next draw/dispatch that uses this bind
	- [ ] sync: match previous and next draw/dispatch and try to find
	      matching sync in between? or something like that

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

vertex viewer/xfb:
- [ ] allow to select vertices, render them as points in the viewport
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

- [ ] proper descriptor resource management. We currently statically allocate
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
- [ ] cbGui: freezing state will currently also freeze the shown commands,
      might not be expected/desired. Reworking this is hard:
	  when freezing state we don't execute any hooks and therefore would need
	  new mechanism to correctly match new records.
	  Fix this when doing the next match/cbGui update iteration (related: next 
	  command group impl iteration)
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
- [ ] The way we have to keep CommandRecord objects alive (keepAlive) in various places
      (e.g. Gui::renderFrame, CommandBuffer::doReset, CommandBuffer::doEnd),
      to make sure they are not destroyed while we hold the lock (and possibly
	  reset an IntrusiverPtr) is a terrible hack. But no idea how to solve
	  this properly. We can't require that the mutex is locked while ~CommandRecord
	  is called, that leads to other problems (since it possibly destroys other
	  objects)
- [ ] could write patched shader spirv data into a pipeline cache I guess
      or maintain own shader cache directory with patched spirv
	  not sure if worth it at all though, the spirv patching is probably
	  not that expensive (and could even be further optimized, skip
	  the code sections).
- [ ] (low prio) our descriptor matching fails in some cases when handles are abused
	  as temporary, e.g. imageViews, samplers, bufferViews (ofc also for
	  stuff like images and buffers).
	  Probably a wontfix since applications
	  should fix their shit but in some cases this might not be too hard
	  to support properly with some additional tracking and there might
	  be valid usecases for using transient image views
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
- [ ] limit device mutex lock by ui/overlay/window as much as possible.
    - [ ] We might have to manually throttle frame rate for window
	- [ ] add tracy (or something like it) to the layer (in debug mode or via 
	      meson_options var) so we can properly profile the bottlenecks and
		  problems
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
	- [ ] would require us to modify shader/pipeline. lots of work
- [ ] the gui code is currently rather messy. Think of a general concept
      on how to handle tabs, image draws and custom pipelines/draws inside imgui
- [ ] reading 64-bit int formats might have precision problems, see the format
      io in util.cpp
- [ ] Implement missing resource overview UIs
	- [ ] sync primitives (-> submission rework & display, after v1.1)
- [ ] show histogram to image in ui. Generate histogram together with min/max
      values to allow auto-min-max as in renderdoc
	- [ ] Using the histogram, we could add something even better, adjusting
	      tonemapping/gamma/min-max to histogram instead just min-max
- [ ] displaying high-res images in small viewer gives bad artefacts
      since we don't use mips. Could generate mips on our own (this requires
	  just copying the currently vieweed mip and then generating our own mips)
- [ ] attempt to retain previous selection in io viewer when selecting
	  new command
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
- [ ] full support CmdDrawIndirectCount in gui (most stuff not implemented atm in CommandHook)
	  {probably not for v0.1} 
- [ ] improve buffer viewer {postponed to after v0.1}
	- [ ] NOTE: evaluate whether static buffer viewer makes much sense.
	      Maybe it's not too useful at the moment.
	- [x] static buffer viewer currently broken, see resources.cpp recordPreRender
	      for old code (that was terrible, we would have to rework it to
		  chain readbacks to a future frame, when a previous draw is finished)
	- [ ] ability to infer layouts (simply use the last known one, link to last usage in cb) from
		- [ ] uniform and storage buffers (using spirv_inspect)
		- [ ] vertex buffer (using the pipeline layout)
		- [ ] index buffer
		- [ ] texel data from a buffer/image copy
	- [ ] ability to directly jump to it - in the contextually inferred layout - from a command?
	      (might be confusing, content might have changed since then)
	- [ ] move to own tab/panel? needed in multiple cases
- [ ] improve image viewer
	- [ ] move to own tab/panel? needed in multiple cases
	      {nah, viewing it inline is better for now}
- [ ] attempt to minimize level depth in cb viewer
	- [ ] when a parent has only one child, combine them in some cases?
	      mainly for Labels, they are currently not too helpful as they
		  make the hierachy just deeper...
	- [ ] allow per-app shortcuts to specific commands?
- [ ] look into imgui shortcuts to allow quick interaction.
      vim-like!
- [ ] we might be able to improve the accuracy of the queried timings (in hooked cbs)
      with inserted pipeline barriers. That will cause certain stages/commands
	  to stall so we can measure the per-stage time in a more isolated environment
- [ ] transfer command IO inspector for buffers
- [ ] transfer commands IO insepctor for ClearAttachmentCmd: allow to select
      the attachment to be shown in IO list and then copy that in commandHook
- [ ] support multiple subresources for transfer commands, images
	- [ ] pain in the ass regarding layout transitions as the range of
	      subresources does not have to be continuous (and we can't assume
		  all of them to be in same layout)
- [ ] important optimization, low hanging fruit:
      CopiedImage: don't ever use concurrent sharing mode.
	  We can explicitly transition the image when first copying it.
- [ ] with the ds changes, we don't correctly track commandRecord invalidation
      by destroyed handles anymore. But with e.g. update_unused_while_pending +
	  partially_bound, we can't track that anyways and must just assume
	  the records stays valid. 
	  We should just not expose any information about that in the gui or
	  state it's limitation (e.g. on hover).
	- [ ] if we absolutely need this information (e.g. if it's really useful
	      for some usecase) we could manually track it. Either by iterating
		  over all alive records on view/sampler/buffer destruction or
		  (e.g. triggered by explicit "query" button) by just checking
		  for all descriptors in a record whether it has views/sampler
		  with NULL handles (in not partially_bound descriptors I guess)
- [ ] we currently copy more levels/layers in commandHook than are shown
      in i/o inspector. Could just copy the currently shown subresource.
- [ ] write tests for some common functionality
	- [ ] format reading/writing; conversion
- [ ] clean up/unify usage of struct/class
	  struct for POD (with no public/private classifiers and member functions),
	  class otherwise I guess
- [ ] move external source into extra folder
- [ ] support timeline semaphores (submission rework/display)
- [ ] support for the spirv primites in block variables that are still missing
 	  See https://github.com/KhronosGroup/SPIRV-Reflect/issues/110
	- [ ] runtime arrays (based on buffer range size)
	- [ ] spec constant arrays
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
- [ ] support compressed/block formats
- [ ] allow to view submissions to a queue
- [ ] implement buffer devicve address
- [ ] implement at least extensions that record to command buffer to allow hooking when they are used
	- [x] push descriptors
	- [x] implement khr_copy_commands2 extension
	- [x] khr fragment shading rate
	- [x] ext conditional rendering
	- [x] ext sample locations
	- [x] ext discard rectangles
	- [x] extended dynamic state
	- [x] ext line rasterization
	- [ ] khr sync commands 2
	- [ ] ext vertex_input_dynamic_state
	- [ ] ext extended_dynamic_state2
	- [ ] ext color_write_enable
	- [x] khr ray tracing
	- [ ] device masks (core vulkan by now)
	      ugh, supporting this will be a MAJOR pain, especially gui rendering.
	- [ ] nv device diagnostic checkpoint
	- [ ] nv exclusive scissor
	- [ ] nv mesh shaders
	- [ ] amd buffer marker
	- [ ] intel performance metrics
	- [ ] nv shading rate image
	- [ ] nv viewport scaling
	- [ ] transform feedback (not sure we want to support this at all?)
	      we probably should support it eventually, just disable our patching
		  for pipes/shaders that use it themselves. Vertex shader won't be available
		  in that case.
	- [ ] nv shading rate enums
- [ ] implement additional command buffer viewer mode: per-frame-commands
      basically shows all commands submitted to queue between two present calls.
	  similar to renderdoc
	- [ ] or just show the most important submission for now? (based on "main
	      submission" heuristics)
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
- [ ] optimize: suballocate and CopiedBuffer
- [ ] optimize: reuse CopiedImage and CopiedBuffer
- [ ] support multiple imgui themes via settings
- [ ] remove PageVector when not used anymore. Maybe move to docs or nodes
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
- [ ] optimization: use custom memory management in QueueSubmit impl
	- [ ] investigate other potential bottleneck places where we
	      allocate a lot
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
- [ ] command groups: come up with a concept to avoid glitchy updates
	  in viewer. Either just update every couple of seconds (lame!) or
	  display something special there.
- [ ] opt: even for command buffer recording we still allocate memory in a lot
	  of places (e.g. CommandBufferDesc::getAnnotate but also in Record/Desc itself).
	  Fix what is possible
- [ ] command groups: should probably also check commonly used handles to match them.
	  at least some handles (at least root resources like memory, samplers etc)
	  will always be in common. Command buffers that use almost entirely the
	  same buffers and images can be considered related
- [ ] register own debug messenger if possible?
- [ ] not sure if current cmdExecuteCommands implementation is the best.
      see comment there.
- [ ] for command descriptions, take pNext chains into account.
	  they are rarely changed just like that (neither is their order I guess)
- [ ] track push constant range pipe layouts; correctly invalidate & disturb
      also track which range is bound for which stage.
- [ ] can we support viewing multisample images?
      either sample them directly in shader (requires a whole lotta new 
	  shader permuatations, not sure if supported everywhere) or resolve
	  into temporary image first (lot of work as well)
- [ ] we might be able to not lock the device mutex for all the time we lock
      the ui (which can be a serious problem) by relying on weak/shared pointers
	  eveywhere (making concurrently happening resource destruction no problem) 
	  	- [ ] probably requires a lot of other reworks as well, e.g. for buffer readback
- [ ] better installing
	- [ ] simple wix windows installer, just needs to install prebuilt layer,
	  	   json file and add the registry file. Should probably also install
	       api header tho
		   (maybe for later, >0.1.0?)
	- [ ] write AUR package (maybe for later, >0.1.0?)
- [ ] general buffer reading mechanism for UI. Implement e.g. to read
      indirect command from buffer and display in command UI
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
- [ ] add optional to just show timing per command (correctly show it per section)
      in command buffer command list.
	  Wouldn't even need to use the (error-prone) command buffer hooking
	  mechanism, could just insert it directly into the forwarded recording
	  commands.
- [ ] improve the case where multiple command buffers are pretty much the
      same and just vary for swapchain image id or something.
- [ ] directly show content for imageview? with correct format/mip/layer etc?
- [ ] (somewhat high prio tho) add support for waiting for command buffer
      recording to finish (with a timeout tho, in which case we simply display
	  that is currently being recorded (and that it takes long)), when viewing
	  it. Especially a big problem for display-window (compared to overlay)
	  when an application re-records in every frame.
	  	- [ ] could be done via conditional variable in command buffer
		      that is signaled on endCommandBuffer
- [ ] handle command-buffer re-recording as graceful as possible.
      	- [x] Try to match selected command in new state
		- [ ] give visual/explicit feedback about re-recording though.
		      maybe show time/frames since last re-record?
			  Show statistics, how often the cb is re-recorded?
- [ ] mode that allows to simply view all commands pushed to a queue?
- [ ] way later: support for sparse binding
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
- [ ] support as many KHR extensions as possible (non-crash)
	- [ ] support UI for them where not too much work
- [ ] support khr ray tracing extension
- [ ] support all other extensions (non-crash)
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
- [ ] optimization: we don't really need to always track refCbs and store
      the destroyed handles. Only do it for submissions viewed in gui?
	  Could just require commandRecords to be valid while selected and
	  then just handle the unsetting logic in CommandBufferGui::destroyed
	    Hm, on a second thought, this won't work so easily since we might
		select a new record that is already invalidated (useful in some
		cases I guess). Also, we want to support showing every command
		for a given handle at some point, where we need this tracking as well.
