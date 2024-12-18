- [x] look into annoying lmm.cpp:138 match assert
- [x] first serialization support
	- [x] create/save handles
	- [x] remove serialize.cpp
	- [x] update gui to new interface
	- [x] use find() for last command level in gui when loading state
		- [x] don't always do it in updateRecords. figure out interface
	- [x] allow to explicitly save relative-commands or gui states,
	      not sure which perspective makes more sense.
		- [x] store in working dir? or in some vil-specific config dir?
		      Guess it would make sense to make them global.
			  And we want global vil settings at some point anyways.
			  So figure out global config dir for this (make configure
			  via env variable but choose good defaults).
			  {nope, working dir for now}
		- [x] Allow to name the saved states
		- [x] Allow to load them, selecting the right stuff.
			- [x] Give meaningful error/warnings messages/stats?
			      {somewhat. Only via dlg_error for now but added debug
				   markers to detect serialization flow mistmatches}
	- [x] continue matching rework/improvements to make sure matching works
	      with loaded handles
	- [x] test: draw/dispatch/traceRays. But also Bind commands.
- [x] fix command viewer update when nothing is selected
- [x] fix resource viewer when switching handle types
      currently does not select the right handle then (e.g.
	  going from Image -> DeviceMemory via refButton)
- [~] add sparse bind matching for frame matching
      {nope, we don't really want/need this. Applications don't submit
	   similar binds over and over again. Should fix/stabilize this via ui}
- [x] hook vkGetDescriptorSetLayoutSupport for sampler unwrapping
- [x] add separate list of bind sparse submissions to queue. See TODO there,
      we currently act like it's dependent on submission order
	  {found better, simpler, implicit way for handling it}
- [x] way later: support for sparse binding
      {LOL at the "way later"}
- [x] sparse binding: fix 'success' assert in insert
      e.g. try bind/flush with texturesparseresidency sample
- [x] sparse: fix semaphore sync tracking stuff
- [x] integrate per-subresource image layout tracking
- [x] remove hardcoded vil api and vil platform toggles
	- [x] env variables instead?
- [x] toupper bug when searching for resource
	- [x] Better resource search (simple fuzzy search)
- [~] test with rainbow six extraction
	  {nvm, vulkan layers are blocked by anti-cheat}
- [x] allow descriptor set dereference in commandRecord, see performance.md
	- fix descriptorSet gui viewer via similar approach
	- [x] only addCow the needed descriptorSets in commandHook
	- [x] capture (via addCow) the descriptorSets when selecting a record
  in the gui
- [x] update enumString.hpp for vulkan 1.3
	- [x] update the generator
- [x] commandHook support for khr_dynamic_rendering.
      find a sample to test it with.
	  basically need some renderpass-splitting-light.
	  Can't use suspend/resume
- [x] expose missing vulkan 1.3 core functions in layer.cpp
	- [x] update dispatch table header and source when they contain them.
	      Don't forget to add the 'aliasCmd' entries in device.cpp.
		  maybe move to extra file/inifunction since it's many by now.
- [x] when viewing an image live in the resource viewer, we just use
      pendingLayout as layout. But we don't lock anymore, maybe there's another
	  submission before gui rendering is done that changes pendingLayout.
	  Instead, assume a fixed layout there and then do a manual transition
	  before/after while the lock is held
	  	- Accessing pendingLayout outside a lock is a race in general.
- [x] look into a lot of descriptor names not being visible anymore
      likely related to spirv-cross update
- [x] fix buffmt for storageBuffer array (crashes atm, does not expect array on that level)
      test with iro, shadowCull
	- [x] a lot of descriptor code was probably never really tested for array bindings.
	      Make sure everything works.
- [x] CommandViewer (and ShaderDebugger probs as well) don't get an initial
      state when ops change and freezeState is active.
	  Solution: instead of *each* component having its own state, they
	  should all directly access the CommandSelection.
	  And unset the state on hook ops change (or maybe even just change the ops
	  *through* the selection? not sure what is better)
	  {quick fix now in, still updating in commandselector when frozen.
	   want a clean solution tho}
	- [x] maybe get rid of CommandViewer::select altogether?
	      or at least only call it on real selection and not on every
		  state update?
- [~] improve gui layout of image viewer (e.g. in command viewer).
      really annoying rn to always scroll down.
	  Maybe use tabs? One for general information, one for the image viewer?
	  {nope, inline is better. Found a better way but just allowing to hide
	   everything else}
- [~] improve image viewer
	- [~] move to own tab/panel? needed in multiple cases
	      {nah, viewing it inline is better for now}
- [x] show histogram to image in ui. Generate histogram together with min/max
      values to allow auto-min-max as in renderdoc
	- [x] Using the image histogram, we could add something even better, adjusting
		  tonemapping/gamma/min-max to histogram instead just min-max
- [x] {later} implement image histograms (probably best like in PIX)
      See minmax.comp and histogram.comp
- [x] fix updateRecord in CbGui
- [x] first viable prototype of local captures:
	- [x] fix varIDToDsCopyMap_ setup, elemID for arrays
	- [x] add names
	- [x] fix/implement updating of local captures
		- [x] allow application to specify whether one capture is enough or
			  if it should be updated
		- [x] for onetime local captures, use a separate list in CommandHook
			  for done ones? to speed up hooking on submission
	- [x] fix hacky UI code, make sure there are no races
		- [x] lots of local locks rn, error-prone
			  introduce cleaner interface, maybe functions on LocalCapture that do it
			  {meh, don't feel like that's a good idea atm. Manual locking
			   when accessing isn't that bad}
	- [x] clean up somewhat hacky CommandViewer and CommandSelector code
		  might need CommandSelector rework to do this properly
		  {yes, please do that}
	- [x] Fix gui/command.cpp assumption on hooks. They assume only one
		  descriptor/attachment whatever is created. Should work with any
		  state that contains the relevant information.
		- [x] first: need to include (set, binding, elem, before) tuple in
			  CopiedDescriptor in CompletedHook state
	- [x] capture attachments and other stuff in local captures
	- [x] make sure local captures work in render passes
	- [x] Fix shader debugger to be able to work with LocalCapture
		- [x] also some hook assumptions, somehow setup varIDToCopyMap_
	- [x] move "ALL THE HOOKS" code out of CommandHook into separate function

- [x] figure out transform_feedback crashes in doom eternal
      crashed deep inside driver in CreateGraphicsPipeline when we patch xfb in :(
      check if it could be a error in patching logic; otherwise analyze our generated spirv,
      see the dumped shaders that might have caused the crash (on D:)
	  {NOTE: check if that was fixed by the xfb changes we did}
	  {yes, does not occur anymore, it seems. Got other crashes now}
- [x] with lockfree gui rendering, the overlay input events in api.cpp
      are racy when QueuePresent is called in another thread.
	  Not trivial to fix. Gui-internal mutex just for that? ugly, deadlock-prone.
	  Just insert events into queue that is processed at beginning of
	  rendering? probably best. Actually, most (all?) events don't need queue.
	  Just update a copy of (mutex-synced) state that is applied at beginning
	  of render.
	  {nope, just used a full-blown queue to match imgui's new queue}
- [x] unref handles in ResourceGui destructor
- [x] UpdateBuffer source not correctly viewable in command viewer
      Debug e.g. with iro preFrame cb
- [x] fix deadlock in HookedSubmission::finish when clearing completed
      hooks while holding mutex
- [x] replace ds::mutex with its pools mutex
- [x] we might be able to improve the accuracy of the queried timings (in hooked cbs)
      with inserted pipeline barriers. That will cause certain stages/commands
	  to stall so we can measure the per-stage time in a more isolated environment
- [x] transfer command IO inspector for buffers
- [x] transfer commands IO insepctor for ClearAttachmentCmd: allow to select
      the attachment to be shown in IO list and then copy that in commandHook
- [x] remove PageVector when not used anymore. Maybe move to docs or nodes
- [x] optimization: use custom memory management in QueueSubmit impl
	- [x] investigate other potential bottleneck places where we
	      allocate a lot
- [~] command groups: come up with a concept to avoid glitchy updates
	  in viewer. Either just update every couple of seconds (lame!) or
	  display something special there.
- [~] opt: even for command buffer recording we still allocate memory in a lot
	  of places (e.g. CommandBufferDesc::getAnnotate but also in Record/Desc itself).
	  Fix what is possible
- [~] command groups: should probably also check commonly used handles to match them.
	  at least some handles (at least root resources like memory, samplers etc)
	  will always be in common. Command buffers that use almost entirely the
	  same buffers and images can be considered related
- [~] not sure if current cmdExecuteCommands implementation is the best.
      see comment there.
- [x] support khr ray tracing extension
- [x] figure out if our linear allocator (the std::allocator) adapter
      should value initialize on alloc
	  {no, clearly not, per spec! See https://en.cppreference.com/w/cpp/named_req/Allocator}
- [x] v0.1, goal: end of january 2021 (edit may 2021: lmao)
	- overlay improvements (especially win32 but also x11; leave wl out)
	- testing & stability improvements
	- docs improvements (mainly: add introduction post explaining stuff)
	- gui improvement: remove flickering data and stuff, allow to get
	  texel values in command viewer
- [x] require wrapping commandBuffers? does it even work otherwise?
      erase from global data when freed
	  {yep, works. We want to keep supporting it for now}
- [x] correctly unset commandBuffer handle on api destruction
- [x] implement additional command buffer viewer mode: per-frame-commands
      basically shows all commands submitted to queue between two present calls.
	  similar to renderdoc
	  {lol, yes, somewhat done by now}
	- [~] or just show the most important submission for now? (based on "main
	      submission" heuristics)
- [x] support compressed/block formats
- [x] implement buffer device address
- [~] optimization: we don't really need to always track refCbs and store
      the destroyed handles. Only do it for submissions viewed in gui?
	  Could just require commandRecords to be valid while selected and
	  then just handle the unsetting logic in CommandBufferGui::destroyed
	    Hm, on a second thought, this won't work so easily since we might
		select a new record that is already invalidated (useful in some
		cases I guess). Also, we want to support showing every command
		for a given handle at some point, where we need this tracking as well.
	  {completely kicked out refCbs by now}
- [x] add optional to just show timing per command (correctly show it per section)
      in command buffer command list.
	  Wouldn't even need to use the (error-prone) command buffer hooking
	  mechanism, could just insert it directly into the forwarded recording
	  commands. {nope, won't do that, hooking isn't that error-prone.}
- [x] improve the case where multiple command buffers are pretty much the
      same and just vary for swapchain image id or something.
- [x] handle command-buffer re-recording as graceful as possible.
      	- [x] Try to match selected command in new state
- [x] general buffer reading mechanism for UI. Implement e.g. to read
      indirect command from buffer and display in command UI
- [x] we might be able to not lock the device mutex for all the time we lock
      the ui (which can be a serious problem) by relying on weak/shared pointers
	  eveywhere (making concurrently happening resource destruction no problem)
	  	- [x] probably requires a lot of other reworks as well, e.g. for buffer readback
- implement at least extensions that record to command buffer to allow hooking when they are used
	- [x] push descriptors
	- [x] implement khr_copy_commands2 extension
	- [x] khr fragment shading rate
	- [x] ext conditional rendering
	- [x] ext sample locations
	- [x] ext discard rectangles
	- [x] extended dynamic state
	- [x] ext line rasterization
	- [x] khr sync commands 2
	- [x] ext vertex_input_dynamic_state
	- [x] ext extended_dynamic_state2
	- [x] ext color_write_enable
	- [x] khr ray tracing
- [~] with the ds changes, we don't correctly track commandRecord invalidation
      by destroyed handles anymore. But with e.g. update_unused_while_pending +
	  partially_bound, we can't track that anyways and must just assume
	  the records stays valid.
	  We should just not expose any information about that in the gui or
	  state it's limitation (e.g. on hover).
	- [~] if we absolutely need this information (e.g. if it's really useful
	      for some usecase) we could manually track it. Either by iterating
		  over all alive records on view/sampler/buffer destruction or
		  (e.g. triggered by explicit "query" button) by just checking
		  for all descriptors in a record whether it has views/sampler
		  with NULL handles (in not partially_bound descriptors I guess)
	  {yeah, we just don't display that in the UI}
- [x] fix CommandBuffer 'view commands' deadlock issue (by making resource
      viewer non-locking)
- [x] fix bug where resources are destroyed while being shown in
      resource viewer in gui
- [x] fix the optimization we have in ds.cpp where we don't store DescriptorSets into the
      hash maps when object wrapping is active (which currently causes the gui to not
	  show any descriptorSets in the resource viewer)
- [x] overlay: looks very different depending on whether the application
      uses srgb or not. Fix that!
		- [x] imgui rendering: in fragment shader, convert to srgb manually
		      when rendering on unorm image

- [x] fix iro:water xfb output drawing issue.
      somehow we draw too many vertices?
	  EDIT: nope, not xfb but vertex input causes issues
	  {fixed, but found the new, related indexed-drawing-truncated-vertex-buffers
	   issue, see next-vertex-iteration section}
- [x] fix vertex viewer input issues (e.g. with a7c)

match rework 2, electric boogaloo
- [x] use LCS in hook already.
  See node 2305 for details.
  The final find operation could then either be LCS (careful! might have
  a lot more commands here than sections for 'match')
  Or - e.g. inside a render pass without blending - be independent
  of the order of draw commands.
	- for action commands inside small (non-solid-renderpass) blocks: do LCS
	- for big blocks: do best-match. In this case something like relID is
	  useful. Maybe build it on-the-fly? should be a lot cheaper, only
	  needed for very few blocks compared to *everything*
	- for order-independent (e.g. no blend/additive) renderpasses we probably
	  want order-independency anyways, so just local-best-match
-> don't hook every cb with matching command. At least do a rough check
      on other commands/record structure. Otherwise (e.g. when just selecting
	  a top-level barrier command) we very quickly might get >10 hooks per
	  frame.
- [x] investigate why conditional rendering vulkan sample is so slow when hooked
      guess: we have *a lot* of sections due to the BeginConditionalRendering
	  calls (a lot of them). Investigate whether matching is working
	  as well and fast as desired
	- check if lower branchTreshold would help
	{gone in release mode but found a couple of bugs related to matching}
- [x] Fix dev.gui modification. Make it threadsafe. E.g. accessed via all
      resource destruction, can happen in any thread. Caused a crash with doom.
	- [x] gui shouldn't be owned by overlay (and therefore swapchain),
		  means we discard its state (e.g. selection) on every resize (when
		  applications don't use the oldSwapchain feature).
		  Should be a member of the device, we only display it once anyways.
		  Create lazily as member there, WITH sync!
- [x] access to dev.swapchain without lock (e.g. cbGui, selector)
      is a race every time. Should be fixed.
- [x] fix hook completed clearing in cbgui, instead use something ringbuffer-like
      in CommandHook
- [x] checkActivate: need to take submission order into account.
      Submission can only be active when all submissions before
	  (on the same queue) are active.
- [x] make sure it's unlikely we insert handles to CommandRecord::invalided
	  since we should be logically able to get around that
	  case (with normal API use and no gui open; i.e. the Record should
	  be destroyed before any resources it uses)
- [x] Get rid of getAnnotate in command buffer end. Find clean solution for
      the problem/improve UI flickering for changing records
- [x] check if we can get rid of refRecords and CommandRecord::invalidated.
      See the point in todo.md:performance about expensive doEnd
	  	- [ ] renderpass
		- [ ] framebuffer
		- [ ] pipeline
		- [ ] image
		- [ ] event
		- [ ] semaphore
		- [ ] query pool
		- [ ] accelstruct (already refCounted)
		- [ ] buffer (already refCounted)
		- [ ] ImageView (already refCounted)
		- [ ] DescriptorSet (already handled I guess)
	- Would also be great because then we can always compare handles (could
	  e.g. take name of destructed handles into account), and don't have
	  this 'invalidated' mess.
	- We could still evaluate whether a command buffer was invalidated:
	  just iterate over all used (referenced) handles and check if any of
	  them was unset
	  	- harder for descriptor sets I guess? maybe store some modificationID
		  for each ds and if was increased (i.e. ds updated) we know
		  cb is invalidted (when ds isn't updateAfterBind).
		  Otherwise, we can look at the ds and check for all handles if they
		  are still valid (with or without refBindings, even though
		  we *might* get false positives without it)
- [x] support for the spirv primites in block variables that are still missing
 	  See https://github.com/KhronosGroup/SPIRV-Reflect/issues/110
	  {no longer using spirv-reflect}
	- [x] runtime arrays (based on buffer range size)
	- [x] spec constant arrays
- [x] static buffer viewer currently broken, see resources.cpp recordPreRender
	  for old code (that was terrible, we would have to rework it to
	  chain readbacks to a future frame, when a previous draw is finished)
- [x] full support CmdDrawIndirectCount in gui (most stuff not implemented atm in CommandHook)
	  {probably not for v0.1}
- [x] limit device mutex lock by ui/overlay/window as much as possible.
      {we don't have monolithic CS anymore now}
    - [ ] We might have to manually throttle frame rate for window
	      {meh}
	- [x] add tracy (or something like it) to the layer (in debug mode or via
	      meson_options var) so we can properly profile the bottlenecks and
		  problems
- [x] (low prio) our descriptor matching fails in some cases when handles are abused
	  as temporary, e.g. imageViews, samplers, bufferViews (ofc also for
	  stuff like images and buffers).
	  Probably a wontfix since applications
	  should fix their shit but in some cases this might not be too hard
	  to support properly with some additional tracking and there might
	  be valid usecases for using transient image views
	  {done via shared ownership now}
- [x] The way we have to keep CommandRecord objects alive (keepAlive) in various places
      (e.g. Gui::renderFrame, CommandBuffer::doReset, CommandBuffer::doEnd),
      to make sure they are not destroyed while we hold the lock (and possibly
	  reset an IntrusiverPtr) is a terrible hack. But no idea how to solve
	  this properly. We can't require that the mutex is locked while ~CommandRecord
	  is called, that leads to other problems (since it possibly destroys other
	  objects)
- [x] can we link C++ statically? Might fix the dota
	  std::regex bug maybe it was something with the version of libstdc++?
	  {tried, but caused more problems than it fixed. Dota bug not fixed,
	   but we have an alternative regex lib now. And are completely
	   avoiding regex in textedit}
- [x] (high prio) figure out general approach to fix flickering data, especially
      in command viewer (but also e.g. on image hover in resource viewer)
	  {fixed pretty much by state freezing and update interval setting}
- [x] also don't always copy (ref) DescriptorSetState on submission.
      Leads to lot of DescriptorSetState copies, destructors.
	  We only need to do it when currently in swapchain mode *and* inside
	  the cb tab. (maybe we can even get around always doing it in that
	  case but that's not too important).
	  For non-update-after-bind (& update_unused_while_pending) descriptors,
	  we could copy the state on BindDescriptorSet time? not sure that's
	  better though.
	- [x] also don't pass *all* the descriptorSet states around (hook submission,
	      cbGui, CommandViewer etc). We only need the state of the hooked command,
		  should be easy to determine.
	- [x] See the commented-out condition in submit.cpp
- [x] {from expensive CommandBuffer::doEnd}
      I guess we could avoid a lot of this 'refRecords' stuff by
	  simple making IntrusivePtr's out of all referenced handles.
	  E.g. make sure Images, Buffers, QueryPools, etc are not destroyed
	  while the CommandRecord is alive. And get completely rid of
	  refRecords and this whole 'invalidated' stuff.
	  OTOH this means it's even harder to find all command usages of
	  a given handle. But we need a different approach for that anyways
	  due to descriptor sets (something like one async search of all
	  known records and then a notification callback for new records/ds
	  or something)
- [x] get rid of annotateRelIDLegacy, use proper context (across cb boundaries)
      when matching commands instead. Should fix CmdBarrier issues in RDR2
	  See docs/own/desc2.hpp. But that isn't enough, we need more context.
	- yeah desc2.hpp isn't too relevant here. In the two places where we
	  still use relID, we instead want to do a local per-block LCS/LMM
	  where we only consider the non-zero matches or something.
	  Or only the two best matches, only do this if we have two candidates?
	  And maybe only in the final hierarchy level?
- [x] improve matching (write tests first), propagate Matcher objects instead
      of floats. See todo in desc.hpp
- [x] improve CmdBarrier matching, see node 2305
      {some improvements done, the general non-action matching rework is still WIP}
- [x] selecting a whole CommandRecord in the command viewer crashed rdr2. Investigate
	  {fixed, was caused by printing record names, might be null tho}
- [x] FIX RENDERPASS SPLIT SYNC. We know this was a problem before.
      But the old solution breaks renderpass compat.
	  Insert barriers as needed when recording instead.
	  Remove the old code from splitInterruptable then.
	  	- wait no, this should work, we record memory barriers
		  between the split passes. Just verify that it works
		  (and figure our a7 bug)
		  {seems like the amd windows driver don't handle
		   memory_{read,write} access flags correctly, as adding
		   color_attachment access helped. Makes no sense, likely
		   just driver bug}
- [x] use mustMoveUnset everywhere a handle might outlive its
      api side lifetime
- [x] improve matching of common commands, e.g. BarrierCmd.
      Returns 0 if they have nothing in command (except second-tier data
	  like dependencyFlags or something). There should be at least
	  one common handle and transition
- [x] figure out why spirv-cross is sometimes providing these weird names
	  (e.g. for buffers; something like _170_2344) instead of simply having
	  an empty alias string
- [x] improve design for buffer viewer. Way too much space atm, make more compact
- [x] add mutex to descriptorPool, make sure it's correctly used everywhere
- [x] fix invalidateCbs race (see command/record.cpp TODO in tryAccessLocked)
- [x] don't give each DrawCmdBase/DispatchCmdBase/RTCmdBase their
      own state copy. Instead just use a pointer and re-allocate
	  on change (in the record memory). Should make records a lot smaller
	  when there are many draw/dispatch calls. And draw/dispatch
	  recording faster since we copy less data.
- [x] fix msvc unit test issue (see CI, only in debug)
- [x] maybe don't add to refRecords of used descriptorSets? We don't
      reference them directly in a record anyways. We still want them
	  in CommandRecord::handles though, not sure how this works. Maybe
	  use some special sentinel values (next == prev == this) to signal
	  this is not linked to the handle itself?
	    Maybe we later on want to see all CommandRecords using a given
		ds? But that's likely a gui operation where we could
		use iterate over all known command records or something.
- [x] in CommandHook::hook only addCow for the descriptorSets that are really
	  needed for the hooked command. We don't need the others (and it could
	  be many)
- [x] make ThreadContext alloc and CommandRecord alloc consistent
	  Allow LinAllocator to take parent memory resource.
	- [x] Also clean up CommandRecord alloc in general. Messy with all those
	  overloads for CommandBuffer
	- [x] Maybe we can reuse code? The allocators are fairly similar
- [x] fix found bufparser leak
	- [x] afterwards: use valgrind for tests in ci to see new leaks/issues?
- [x] clean up ThreadContext (after inlining rework)
	- [x] update to better ThreadContext version, where we don't use stdcpp alignment
- [x] figure out tracy issues. On windows, it causes problems with a lot
      of applications that I can't explain :( Some of the problems
	  have been caused by a tracy update, it used to work!
	- [x] Might be fixed now, test with doom on windows
	      (on windows we currently init the ThreadContexts on thread creation, meaning
	       we can't use tracy there. Otherwise it runs)
- [x] spvm: add callback for getting image data instead of requiring the whole image
      to be present
- [x] spvm: Avoid copies for setting buffer data.
      Maybe just add callback when a buffer is accessed that can return the data?
	  I guess the only ways for access is OpLoad, optionally via OpAccessChain.
	  Something like `spvm_member_t get_data(spvm_result_t var, size_t index_count, unsigned* indices)`
	  Alternative: a more specific interface only for the most important case, runtime arrays.
	  (But honestly, shaders could also just declare huge static arrays so we probably
	  want the general support).
- [x] higher-id descriptor sets sometimes incorrectly unbound, e.g. iro/atmosphere
      {found the bug, binding a descriptor to a previously unbound slot would
	   always disturb all following ones even though that's incorrect}
	- [~] add unit test for this case.
	      {we just disabled this whole disturb tracking for now at all}
- [x] proper support for reading depth, packed and compressed formats.
      See TODOs in ioFormat
      ImageViewer does not handle depth/stencil aspects correctly when
	  reading back texels
	  {EDIT: don't need this anymore, we just use sampling to read images}
	- [~] write some tests
- [x] image viewer: fix display of HUGEx1 images
- [x] instead of copy and cpu-formatting of texture values, we should
      probably just dispatch a single compute shader invocation that samples
	  the texture and writes the float4 output to a buffer.
	  See readTex.comp
- [x] We export dlg and swa symbols that we shouldn't export! See
      nm -gDC --defined-only --numeric-sort libVkLayer_live_introspection.so | less
- [x] when the applications creates a resource with usage_exclusive
	  and we overwrite it to concurrent, we modify the queue ownership
	  transitions. But this breaks when queue ownership transition is combined
	  with layout transition. The application will then do the same transition
	  twice with our hooks. Not sure about proper solution yet, somehow
	  filter out on of the transitions?
- [x] device.cpp creation: handle case that vulkan12 is not available
      and that VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2 was not enabled
- [x] for debugging: track size of VkDeviceMemory objects created for
      CommandHook objects
- [x] image viewer: fix layer selection
- [x] image viewer validation error when we don't hover the image
      (happens when mip > 0 is selected)
	  {should be fixed with new image viewer}
- [x] imgui input: figure out why del key does produce a '?'.
	  e.g. in buffer viewer {nvm. issue in swa/iro}
- [x] improve handling of transparent images. Checkerboard background?
	- [x] when viewing image as grayscale they become transparent atm.
	      no idea why
	- [x] also don't apply scale for alpha
	- [x] maybe add some "transparent" background (some known pattern)
- [x] IO viewer additions
	- [x] start using src/gui/command.hpp
	- [x] fix descriptor arrays
	- [x] use the new buffer viewer for transfer buffer commands
		- [x] fix `[cb.cpp:1056] assertion 'found' failed` for cmdUpdateBuffer,
			  i.e. support buffer IO viewing for transfer commands
	- [x] fix ClearAttachmentCmd handling, allow to copy/view any of the cleared attachments
	- [x] when viewing attachments, show framebuffer and image/imageView (see TODO in code)
	- [x] when viewing transfer img, show image refButton
	- [x] adapt ioImage_ to selected image (e.g. channels)
		- [x] also fix logic for depthStencil images. Select depth by default.
			  (can be tested with doom)
	- [x] support texel reading implementation for cb-viewed-images and clean
		  up color format presentation
- [x] improve buffer viewer UI.
	- [x] integrate https://github.com/BalazsJako/ImGuiColorTextEdit
	- [x] use monospace font
	- [x] refactor parsing code to correctly output errors and warnings.
	      - Idea here was to use two types of exceptions: one for expected
		    errors and one for failed asserts (that might happen often in the
		    beginning or when adding new features to the lang;
		    we want to write-out a dot file in that case and don't
		    crash the application).
		  - accumulate warnings inside Parser struct and just return them.
		    For instance: badly aligned data, column-major matrices etc
	- [x] show errors and warnings {no warnings for now}
- [x] fix general commandHook synchronization, see design.md on
      buffer_device_address, uncovered general potential race
- [x] in CopiedImage::init: check for image usage support
	- [x] generally: allow the image copy operation to fail.
- [x] add support for timeline semaphores. Should just require some simple
      changes to submission logic when we use them inside the layer.
	  Test with official samples repo
	  {couldn't find an issue with them anymore}
- [x] remove -DUNICODE from defines
	- [x] also check that vil_api works either way, can't depend on that
- [x] fix "command not found 4 frames" shortly appearing when selecting new command
      {fixed by simply increasing the threshold for showing the message,
	   might come back to haunt us later on :)}
- [x] correctly integrate spirv-cross everywhere, remove spirv_reflect
	- [x] correctly set specialization constants before using it for reflection
	      reset previously set constants to default. Not sure how tho.
	- [x] figure our sync for spec constants. I guess we should have a mutex
	- [x] remove util/spirv.h
- [x] we should likely switch to spirv-cross instead of spirv-reflect
	- we will probably need some its functionality later on anyways
	- already hit some hard limitations of spirv-cross that would require
	      a lot of changes
- [x] Unselect a command when new frames don't contain it any more?
      We will still view the old state at the moment. Unselecting can
	  be a problem since we might want to continue viewing the stale
	  state if the command gets only submitted once every N frames.
	  Maybe show a visual hint that the state is stale though?
- [x] CommandBufferGui: separate the RecordBatches that are shown as commands
      and the RecordBatches that the currently selected command is part of.
	  We need to store the latter to correctly do contexted LCS
- [x] Make renderPass intrusive ptr object (like imageView etc) and directly
      embed RenderPassDesc. Correctly link to it from framebuffer/gfxPipeline
- [x] respect ds dynamic offsets. Test with SW sample
- [x] fix IO viewer being stuck because we can't get a new command
      {still not fixed, even when using LCS & new update logic}
	- [x] fix dlg_assert(found), gui/cb.cpp
	- [x] use/implement LCS and better general strategy when in swapchain mode for
		  command matching/finding the best hook from last frame
- [x] alternative view of DeviceMemory showing a better visualization of
      the resources placed in it (and the empty space)
- [x] proper shipping and installing
	- [x] make the json file a config file, generated by meson
	- [x] write wiki post on how to build/install/use it
	- [x] fix for vil_api.h: should probably load *real* name of library (generated
	      via meson), not some guesswork.
	      Important on windows, to support all compilers. See TODO on lib name there.
		  OTOH this would make vil_api.h depend on some generated stuff which
		  is bad as well.
		  {added a define}
- [x] detect when variables like Builtin CullDistance are present in spirv
      but never written - we don't have to show them in the xfb tab.
	  We don't even have to capture them in the first place.
- [x] vertex viewer: better tabular display of data; also show indices. Respect offset
- [x] vertex viewer improvements
	- [x] automatically compute bounding box of data and center camera
	- [x] allow showing active frustum
- [x] vertexViewer: support drawIndirectCount in vertex viewer
	- [x] similar: support drawIndirect with multiple commands
- [x] vertexViewer: also support just showing a single draw command of multiDraw
	  (see the other todo on showing attachments/ds state per-draw)
- [x] fix cbGui freeze: temporarily unfreeze when selecting a new command
      I guess we can only handle this in the cbGui itself. Just set a flag
	  that commandHook.freeze is set to false until we get a new state
- [x] the shown commands (of vkQueueSubmit) are not updated when no command
      is selected?
- [x] xfb: consider spec constants. Store them in created module, might
      hook xfb multiple times, for different spec constants.
- [x] xfb, vertex viewer: consider dynamically set stride when pipeline has that dynamic state
- [x] xfb: support custom outputs, not just the Position Builtin
- [x] fix 3D vertex viewer for 2D position data (needs separate shader I guess)
	  {nope, it doesn't. Using vec4 input with r32g32_sfloat attribute is valid}
- [x] (easy) don't lock the dev mutex during our spirv xfb injection.
      Could e.g. always already generate it when the shader module
	  is created.
- [x] fix hooking commands inside CmdExecuteCommands
	  I guess CmdExecuteCommands should not do anything in record()?
	  We have to watch out for extensions there though
- [x] the current situation using imgui_vil is terrible. We need this to make
      sure that imgui symbols we define don't collide with the symbols
	  from the application. The proper solution is to set symbol_visibility
	  to hidden. But then we can't test vil. Maybe just export the stuff
	  we test explicitly? Same for spirv_reflect basically.
	  {can be removed in future, we don't export symbols anymore}
- [x] ditch spirv_reflect for spirv_cross. Better maintained, support lots of features
      that we need that spirv_reflect does not
- [x] commandHook: See the TODO on 'completed'. Might create problems atm.
- [x] fix commandHook for updateAfterBind, updateUnusedWhilePending.
      We might need to invalidate the hooked records when a used descriptor
	  was changed
- [x] the performance notes in CommandHook::hook are relevant
- [x] rework dev/gui so that there is never more than one gui. Supporting
      multiple guis at the same time is not worth the trouble (think
	  about command buffer hooking from multiple cb viewers...)
	- [x] what to do when window *and* overlay is created? or multiple overlays?
		  Should probably close the previous one (move gui object)
		  See todo in Gui::init. Make sure there never is more than one
- [x] should support image-less framebuffer extension as soon as possible,
	  might need a lot of changes {it didn't but uncovered a bug in secondary cbs}
- [x] Add more useful overview.
	- [x] Maybe directly link to last submitted command buffers?
	      {this is kinda shitty though, need the concept of command buffer groups
		   to make this beautiful}
	- [x] show graph of frame timings (see first sketch swapchain header)
	- [x] show enabled extensions
	- [x] show enabled features
	- [x] only show application info if filled out by app. collapse by default?
- [x] implement mechanism for deciding per-handle-type whether object wrapping
      should be done. Either via GUI or env var.
	  Performance-wise it's only really important for
	  CommandBuffers and DescriptorSets (technically also for Device but
	  that pretty much guarantees that 50% of new extensions will crash).
- [x] bump api version as far as possible when creating instance?
      not sure if anything could go wrong in practice
- [x] insert command buffer timing queries
- [x] track query pools
- [x] track buffer views
- [x] should probably not be possible to ever unselect ParentCommands in
      cb viewer (CommandTypeFlags). Just always display them?
- [x] CommandMemBlock alignment handling might currently be wrong since
      we don't take the alignment of the beginning of the mem block into account,
	  might be less than __STDCPP_DEFAULT_NEW_ALIGNMENT__ due to obj size.
	  Handle it as we do in ThreadMemBlock
- [x] when we select a resource of type X should we set the current filter to X
      in the resource gui? Can be somewhat confusing at the moment
- [x] figure out how to correctly match swapchain.frameSubmissions to the
	  commandHook state. See TODO in cb.cpp `dlg_assert(found)`
- [x] when there are multiple commands with the same match value, choose
      the one with the smaller difference in Command.relID?
	  Or (more properly; harder to determine) just go by order then.
- [x] make sure the environment variables for overlays/window creation work
      as specified in readme everywhere
- [x] resource viewer: we don't know which handle is currently selected
      (in the window on the left), can get confusing when they don't have names.
	  Should probably be an ImGui::Selectable (that stays selected) instead
	  of a button
- [x] remove CommandGroups? Just rely on command matching instead?
      figure out how to properly do matching across command buffer
	  boundaries; taking the context - the position inside a frame - into account
- [x] currently command groups *interfer* with our new, improved command matching
      since it may not recognize two records as being in the same group
	  when they should be (and we would find a perfect command match).
	  Another point for removing them in their current form; as a requirement
	  for command matching
	  {yep, just removed them}
- [x] make sure we fulfill multithreading requirements for
      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT and friends
	  {should work with out current DescriptorSet, State separation}
- [x] (important) the way we copy/modify descriptor set states might have
      a data race at the moment. When copying, we might try to increase
	  the reference count of an already destroyed State object (when it is
	  currently being replaced?)
- [x] (important) the way we currently copy vectors of IntrusivePtr handles
      outside the lock (e.g. in Gui) to make they don't get destroyed inside
	  isn't threadsafe
- [x] {high prio} the many shared locks when recording a cb can be harmful as well.
      We could get around this when wrapping handles, should probably
	  look into that eventually (maybe allow to switch dynamically at
	  runtime since wrapping makes supporting new extensions out of the
	  box highly unlikely so users could fall back to hash tables at
	  the cost of some performance).
- [x] identified bottleneck (e.g. with dota): the device mutex for DescriptorSetState.
      Should be possible to give each DescriptorSetState its own mutex.
	  Hm, might still be a problem that we update so many links between
	  resources (ds <-> cb and ds <-> view).
	  Could still try to use a linked grid for those links, could possibly
	  even make that completely lockfree.
	  Maybe we can even get rid of ds <-> cb links? That would help.
	  - [x] ideally, we would have no (non-local) locks during descriptor
			set updating {well, duh, we still have to lock when creating
			a new state object but that should be fine as it shouldn't
			happen often}
	- [x] ideally, we would have *no* locks during command recording at all
	      {almost there, have no locks for most commands}
- [x] add tracy for profiling
- [x] clean and split up QueueSubmit implementation. It's way too long,
      does way too much. And will probably further grow
	- [x] also: always check in the beginning for finished submissions
- [x] when there is more than one record of the same group in one RecordBatch we get
      into troubles when viewing it in swapchain commands mode.
	  The problem is that *all* group submissions update hook.state and we might end
	  up viewing one of those states, not really the one we want.
	  Which can lead to weird issues, e.g. the gui shows a depthStencil attachment
	  but when we click it we see a color attachment. Not sure how to really fix this,
	  probably use more information in swapchain mode (e.g. submission order) and
	  improve matching in general (e.g. make sure that render pass attachments *must* match
	  for command group/records matching)
	  {~semi-done with the new command matching system, still missing
	   better command groups and command group sequence matching, but
	   there are new todos for that}
- [x] proper rasterization introspection using transform feedback.
      we already query in the device whether it's supported
	- [x] requires SPIR-V hooking (only execution mode setting and adding xfb to outputs)
	      have to do it to all pipelines though I guess.
		  Not supported with multiview, respect that (via renderpass)
	- [x] capture in on our side into buffers
	- [x] not sure if we need the transform feedback queries or if we
	      can always deduce that information statically
		  {we have the information statically in all cases so far}
- [~] might be better to determine command group at EndCommandBuffer
      instead of first submission. We can't use the used queue though...
	  {nope, it's not, the command group concept is mainly a hack in the
	   first place}
- [x] stress test using a real vulkan-based game. Test e.g. with doom eternal
	- [x] vkQuake2
	- [x] doom eternal
		- [x] figure out how to make multi-submission drawing easily viewable
		      -> per-frame-commands cb viewer mode
	- [x] dota 2 (linux)
	- (also tried out rdr2 in the meantime)
- [x] before release: test on windows & linux, on all owned hardware
- [x] Allow to freeze state for current displayed command, i.e. don't
      update data from hook
	- [x] figure out how to communicate this via gui.
	      This is a distinct option form the "displayed commands source" and UpdateMode
	- [x] While at it, clean up all the hook logic for io viewer
		  {refactored to gui CommandViewer}
- [x] test `splittable` impl for render passes. There are very likely issues.
      (especially for the cases where render pass can't be split)
	  {see docs/test/rpsplit.cpp, seems to work in basic cases}
- [x] fix debug utils label hierachy. They could have been started/completed
      in different command buffer
	  {note that there is still a remaning issue as we can't currently
	   track queue-pushed/popped lables correctly, see todo.md}
- [x] allow to select in cb viewer which commands are shown
	- [x] make that more compact/intuitive if possible
	- [x] looks really ugly at the moment, improve that.
	      maybe move to own settings tab? Wouldn't expect people to change
		  it often tbh
	- [x] cleanest would probably a button that spawns a popup/dialog
	      in which this can be selected. That is possible with ImGui,
		  see BeginPopup.
		  Alternatively move it to a general settings tab (that we kind of
		  need by now).
	- [x] Improve the "Freeze state" checkbox, it vastly out of place rn
- [x] fix resource viewer
	- [x] fix filtering by type
	- [x] fix filtering by name
- [x] xfb: use heuristic to figure out if ortho or perspective projection is used
	- [x] and then use the matching shader (i.e. scaled w vs scaled z as view-space z coord)
	- [x] probably best to have one vertex shader controlled via push constant
- [x] fix Gui::draws_ synchronization issue
	  See Gui::pendingDraws (called from queue while locked) but also
	  Gui::finishDraws (e.g. called from CreateSwapchain without lock,
	  potential race!)
	  {NOTE: there are likely a couple more sync issues for Gui stuff}
- [x] make queues viewable handles
	- [x] allow to view command groups per queue
- [x] take VkPhysicalDeviceLimits::timestampComputeAndGraphics into account
	  for inserting query commands (check for the queue family in general,
	  we might not be able to use the query pool!).
	  {we don't need that limit, it's a general guarantee. We just check per-qfam}
- [x] in io & resource viewer: mip slider broken
	- [x] Also move to own line? or just make half width?
- [x] Rename FUEN_ macros into VIL_
- [x] automatically update resource lists in resource gui when tab is re-entered
      from somewhere else
- [x] improve enumString
	- [x] make enumString.hpp return some deafult value ("" or "<?>") instead of nullptr.
		  Could cause crashed for future values atm
	- [x] better enumString.hpp. Remove prefixes
	- [x] get VK_ERROR_UNKNOWN into enumString.hpp (and check if other enum values are missing for some reason)
	      (the vk.xml we used was just too old. Shows how important updating it)
- [x] always use nearest sampling for images?
- [x] add example image to readme (with real-world application if possible)
- [x] probably rather important to have a clear documentation on supported
      feature set, extensions and so on
	  {see docs/features.md}
	- [x] clearly document (maybe already in README?) that the layer
	      can crash when extensions it does not know about/does not support
		  are being used.
	- [x] update README to correctly show all current features
- [x] proper layout of child windows with resizing
      See https://github.com/ocornut/imgui/issues/319
	  {just switched to the new imgui tables, they fix issues}
- [x] show more information in command viewer. Stuff downloaded from
      device before/after command
	  {some TODOs of this feature moved to after v0.1}
	- [x] new per-command input/output overview, allowing to view *all* resources
	- [x] I/O inspector for transfer commands
	      {left buffer gui io support for later}
	- [x] add more information to I/O viewer (especially for images)
	- [x] fix code flow. Really bad at the moment, with Commands calling
	      that displayAction function and optionally append to child.
	- [x] improve the new overview. See all the various todos
	      {still lots of TODOs left though, just most important ones fixed now}
	- [x] chose sensible default sizes/layouts
	- [x] implement buffer viewer (infer information from shaders)
	- [x] factor out image viewer from resources into own component; use it here.
	      Allow layer/mip selection
	- [x] re-add timing display in command inspector
	- [x] display arrays in buffers correctly
	      {moved to spirv todo}
	- [x] for storage buffers/storage images, a before/after/change
	      view would be really nice. We can do that.
- [x] Fix the (due to table now broken) append child logic in displayInspector
	- [x] should likely call displayActionInspector directly from inside cbGui
	      and only show command inspector itself when "Command" is selected
- [x] selecting cmdBeginRp in gui gives validation layer error.
      I suspect we try to split it or something?
	  {nope we didn't split but still inserted splitting barrier, not allowed}
- [x] Link to swapchain in swapchain images
- [x] next ui sync rework
	- [x] don't lock device mutex while waiting for fences (see waitForSubmissions)
	- [x] use chain semaphores for input
	- [x] correctly sync output, but only if it's needed (might work already)
	      {i guess is application calls vkQueuePresentKHR we trust it knows
		   what it is doing?}
	- [x] if timeline semaphores are available, use them! for all submissions (in and out)
		- [x] when timeline semaphore extension is available, enable it!
	- [x] insert barrier at end of each submission, optimize case where application
	      uses same queue as gui
- [x] implement overview as in node 1652
      {didn't implement all of it, multiple swapchains is a feature for later}
	- [x] associate CommandGroup with swapchain (and the other way around?)
	- [x] allow something like "update from swapchain" in command buffer viewer?
	      It seems to me we want a more general "command source" concept
		  for the command buffer viewer. Could be queue/command buffer/command group/
		  swapchain/identified per-frame submission/fence-association or
		  something like that.
- [x] add explicit "updateFromGroup" checkbox to command viewer
	- [x] we definitely need a "freeze" button. Would be same as unchecking
	      checkbox, so go with checkbox i guess
	- [x] do we also need an updateFromCb button?
		  {nvm, reworked cb update settings ui}
- [x] find a way to limit number of command groups. Erase them again if not
      used for a while. don't create group for single non-group cb submission?
	  Or somehow quickly remove again
- [x] barrier command inspectors: show information about all barriers, stage masks etc
	- [x] CmdPipelineBarrier
	- [x] CmdSetEvent
	- [x] CmdWaitEvents
- [x] destroying a sampler should also invalidate all records that use
      a descriptor set allocated from a layout with that sampler bound as
	  immutable sampler. No idea how to properly do that, we need a link
	  sampler -> descriptorSetLayout and additionally
	  descriptorSetLayout -> descriptorSet. Or maybe implicitly link
	  the sampler as soon as the descriptor set is created (from the layout,
	  in which it is linked) and then treat it as normal binding (that
	  is never invalidated, treat as special case)
	- [x] make sure to never read layout.pImmutableSamplers of an invalidated
	      record then. Destroying the immutable sampler would invalidate the
		  ds, causing the ds to be removed from the record.
- [x] fix command hook synchronization issue where we use a CommandHookState that is currently
      written to by a new application submission. I guess we simply have to add something like
	  "PendingSubmission* writerSubmission {}" to CommandHookState that is set every time
	  the state is used in a hooked submission and unset when the submission is finished.
	  When we then display (reading a buffer is fine, we copied them to separate memory
	  and are not using the mapped memory directly anyways) from a CommandHookState
	  and `writeSubmission` is set, we chain our gui draw behind it.
- [x] IMPORTANT! keep command group (or at least the hook?) alive
	  while it is viewed in cb viewer? Can lead to problems currently.
	  Unset group in kept-alive records? We should probably keep the
	  group alive while a record of it is alive (but adding intrusive
	  pointer from record to group would create cycle).
	  Are currently getting crashed from this.
	- [x] re-enable discarding old command groups when this is figured out.
- [x] cleanup comments in rp.hpp. Currently a mess for splitting
- [x] debug gui.cpp:1316,1317 asserts sometimes failing (on window resize)
	  {we forget to check draw.inUse again before calling it in Gui::finishDraws)
- [x] setup CI for windows (msvc and mingw) and linux
- [x] better display (or completely hide?) swapchain images
	  We should probably fill-in Image::ci for them.
- [x] fix "unimplemented descriptor category" bug (not sure when it appears)
      {we were casting from descriptor type to descriptor category in stead
	   of using the function...}
- [x] Remove virtual stuff from CommandBufferHook
- [x] optimization (important): when CommandRecord is invalidated (rather: removed as current
      recording from cb), it should destroy (reset) its hook as it
	  will never be used again anyways
	  	- [x] see TODOs in CommandHookRecord (e.g. finish)
- [x] support integer-format images (needs different image display shader)
- [x] when checking if handle is used by cb, consider descriptor sets for images, buffers & bufferViews!
      when it's checked for an image/buffer, consider all descriptor of views as well.
- [x] copy vulkan headers to vk/. So we don't rely on system headers
- [x] remove src/bytes in favor of util/bytes.
- [x] move other util headers to util
- [x] A lot of sources can be moved to src/gui
	- [x] rename imguiutil. Move to gui
- [x] figure out why we can't name handles from inside the layer
	  {eh, see ugly workaround for now in device creation}
- [x] fix ui for fixed resource tracking: check for nullptr resource references
      everywhere ~~(and use weak/shared pointer where we can't manually reset to null)~~
	- [x] use CommandBufferRecord::destroyed to show <destroyed> instead of
	      resource reference buttons
- [x] argumentsDesc for transfer commands (missing for a lot of commands rn)
- [x] fix nonCoherentAtom mapped memory flushing
- [x] fix/disable render pass splitting for transient attachments
	- [x] think of other cases where it might not work.
- [x] move old commandHook concept to docs/stash. Or just delete
      {last commit before remove: f140de13aed126311fb740530181af05cbc7a651}
- [x] before copying image in renderpass in commandHook, check if transferSrc
      is supported for image (we might not be able to set it in some cases)
	  	- [x] check for support in swapchain and image creation
- [x] cleanup, implement cb viewer as in node 1652
- [x] in overview: before showing pending submissions, we probably want to
      check them all for finish
- [x] fully implement command buffer viewer
	- [x] support all vulkan 1.0 commands (add to cb.h and commands.h)
	- [x] show all commands & info for commands
- [x] fix dummy buttons in command viewer (e.g. BeginRenderPass)
- [x] track dynamic graphics pipeline state
	- [x] show it in command ui
- [x] improve/cleanup pipeline time queries from querypool
	- [x] query whole-cb time, and correctly support querying for full renderpass
	- [x] we should probably show estimate of time range. The current values
	      often become meaningless on a per-command basis.
		  Also average them over multiple frames, avoid this glitchy look
- [x] properly implement layer querying functions
	- [x] version negotiation?
	- [x] implement vkEnumerateInstanceVersion, return lowest version we are confident to support.
		  maybe allow to overwrite this via environment variable (since, technically,
		  the layer will usually work fine even with the latest vulkan version)
		  EDIT: nope, that's not how it is done.
- [x] restructure repo
	- [x] add an example (using swa)
	- [x] move everything else into src (maybe api.h to include/?)
	- [x] decide on license and add it
	      pro GPL: no one has to link this layer so it would not have a negative
		    impact on anyone. And using GPL it would prevent abusive usage (such as
			forking/privately improving and selling it)
		  pro MIT: companies are probably still wary about using GPL software
		    and i totally don't have a problem with this being used for
			proprietary software development (such as games).
			But otoh, companies not understanding licensing and open source
			should not be my problem.
		  {yep, going with GPL for now}
- [x] always try to enable swapchain extension on device creation
- [x] name our internal handles for easier debugging
- [x] switch to shared pointers for device handles, keeping them alive
	  NOTE: nope, not doing that for now. Explicit resource connection tracking
	  implemented though.
- [x] store for handles in which command buffers they were used and set the
      command buffer to invalid state when they are changed/destroyed
- [x] cleanup Renderer/Gui implementation: merge back together
	- [x] proper gui sync implementation
	- [x] move gui tabs into own classes
- [x] display command buffer state in resource UI
- [x] use better enum->string helper. The vk_layer one has several problems
	  probably best to just modify their python script and put it into docs/.
	  Or use custom vkpp output generator?
	  {went with custom vkpp output generator, easy to write & maintain given
	   the extensive registry parser}
- [x] display in UI whether resources are destroyed or not
	  {NOTE: nvm, we decided against shared_ptr approach and never have destroyed resources}
- [x] fix bug for cmdExecuteCommands when executed command buffers are invalid/destroyed
- [x] Remove Device::lastDevice api hack. Instead return a dev handle from fuenLoadApi
      Should probably just store it inside the api struct.
- [x] fix destructors: vulkan allows null handle, we don't
- [x] fix push constant tracking in command buffer
- [x] fix refCbs handling. We might not be able to do it that way.
- [x] rename cbState.hpp -> boundState.hpp? or just bound.hpp?
- [x] correctly handle secondary command buffers
	- [x] might need adaptions for render passes, bound state and such
		  {from a first look, no does not seem so. State is reset at cb boundary.
		   We just can't assume that something like cmdDraw is inside a render
		   pass for secondary cbs but we don't do that anyways}
- [x] Switch to a more useful fork/branch of vkpp: Generate vk::name
      functions for plain vulkan enums, don't use anything else here.
	  Probably best to not even include vkpp as subproject, just copy
	  /dispatch and /names here.
	  	- [x] nvm, should probably just ditch vkpp all together and use the layer utils
		- [x] check if we can use more of the the layer utils
		      maybe we can replace our own hash table?
			  {nah, not worth it for now, using std works fine, opt for later}
- [x] fix our global dispatchable handle hash table. Either use the vk_layer
	  one or remove the type hashing (dispatchable handles are globally unique).
- [x] we have to check in barrier commands whether the image (edit: or buffer)
      was put into concurrent mode by us, and if so, set queue families to ignored
	  (otherwise it's a spec error)
- [x] correctly store pNext chain when recording command buffers.
      (alternative: at least set them to nullptr...)
- [x] improve UI
	- [x] Add proper image viewer
	- [x] Add buffer viewer
- [x] full support of all vulkan 1.0 commands (except sparse binding I guess)
	- [x] should probably also support the easy-to-support extensions
	      for resource creation already. At least widely used/important extensions
		  {yep, we should support the most critical 1.1 and 1.2 stuff, except descriptor update templates}
- [x] support descriptor update templates
- [x] properly shutdown everything, no leftover resources and layer warnings
- [x] proper queue creation and querying for window display
- [x] properly shut down rendering thread for own-window display
- [x] test display window for compute-only applications
	- [x] come up with something smart to block them before they shut down.
	      Is there a sensible way to do this in the layer or should applications
		  do it themselves? **write wiki post**
		  We could simply block the application in vkDestroyDevice? but then,
		  everything is already destroyed I guess. Don't see a way to do it rn.
		  Applications otoh just have to insert a single std::getchar before
		  terminating, a lot easier.
		  {see docs/compute-only.md}
- [x] clean up currently slightly hacky window-thread communication.
      (and all instance stuff in layer.cpp)
      ~~investigate whether we have to create the display in that thread
	  on windows already as well~~
	- [x] related, swa/winapi: don't create dummy window when wgl is disabled?
	      (does not matter, isn't a problem with our fixed threading)
- [x] figure out a general policy to transitive handle-adding to command buffer
	- [x] e.g.: when an descriptor is used, is the imageView added to handles?
	      the image as well? the memory as well?
	      {yes, this is probably the expected and best way}
	- [x] add handles transitively for cmdExecuteCommands?
	      {yes, this is probably the expected and best way}
- [x] optimize memory consumption in cbs.
      the UsedHandle::commands vector are over-allocating *so much* currently,
	  maybe replace them with linked lists (non-intrusive)?
- [x] implement command group concept and last command buffer state viewing
