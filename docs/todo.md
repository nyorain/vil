# Todo

v0.1, goal: end of january 2021

- Cleanup
- Docs
- Command-introspection
- Sync rework
- Testing, Profiling, Needed optimization

- [ ] setup CI for windows (msvc and mingw) and linux
- [ ] better pipeline display in resource viewer
- [x] when checking if handle is used by cb, consider descriptor sets for images, buffers & bufferViews!
      when it's checked for an image/buffer, consider all descriptor of views as well.
- [ ] figure out "instance_extensions" in the layer json.
      Do we have to implement all functions? e.g. the CreateDebugUtilsMessengerEXT as well?
- [ ] barrier command inspectors: show information about all barriers, stage masks etc
	- [ ] CmdPipelineBarrier
	- [ ] CmdSetEvent
	- [ ] CmdWaitEvents
- [ ] I/O inspector for transfer commands
- [ ] support CmdDrawIndirectCount in gui
- [ ] show more information in command viewer. Stuff downloaded from
      device before/after command
	- [ ] new per-command input/output overview, allowing to view *all* resources
	- [ ] fix code flow. Really bad at the moment, with Commands calling
	      that displayAction function and optionally append to child.
	- [ ] improve the new overview. See all the various todos
	- [ ] chose sensible default sizes/layouts
	- [ ] implement buffer viewer (infer information from shaders)
	- [ ] factor out image viewer from resources into own component; use it here.
	- [ ] re-add timing display in command inspector
- [ ] proper layout of child windows with resizing
      See https://github.com/ocornut/imgui/issues/319
- [ ] better enumString.hpp. Remove prefixes
- [x] copy vulkan headers to vk/. So we don't rely on system headers
- [ ] Allow to freeze state for current displayed command, i.e. don't
      update data from hook
	- [ ] figure out how to communicate this via gui.
	      This is a distinct option form "updateFromGroup" or "updateFromCb".
- [ ] fix Gui::draws_ synchronization issue
	  See Gui::pendingDraws (called from queue while locked) but also
	  Gui::finishDraws (e.g. called from CreateSwapchain without lock,
	  potential race!)
	  {NOTE: there are likely a couple more sync issues for Gui stuff}
- [x] find a way to limit number of command groups. Erase them again if not
      used for a while. don't create group for single non-group cb submission?
	  Or somehow quickly remove again
- [ ] IMPORTANT! keep command group (or at least the hook?) alive 
	  while it is viewed in cb viewer? Can lead to problems currently.
	  Unset group in kept-alive records? We should probably keep the
	  group alive while a record of it is alive (but adding intrusive
	  pointer from record to group would create cycle).
	  Are currently getting crashed from this.
	  - [ ] re-enable discarding old command groups when this is figured out.
- [ ] implement copy_commands2 extension
- [x] optimization (important): when CommandRecord is invalidated (rather: removed as current
      recording from cb), it should destroy (reset) its hook as it 
	  will never be used again anyways
	  	- [x] see TODOs in CommandHookRecord (e.g. finish)
- [x] add explicit "updateFromGroup" checkbox to command viewer
	- [x] we definitely need a "freeze" button. Would be same as unchecking
	      checkbox, so go with checkbox i guess
	- [ ] do we also need an updateFromCb button?
- [x] allow to select in cb viewer which commands are shown
	- [ ] make that more compact/intuitive if possible
	- [ ] looks really ugly at the moment, improve that.
	      maybe move to own settings tab? Wouldn't expect people to change
		  it often tbh
- [x] make queues viewable handles
	- [x] allow to view command groups per queue
	- [ ] view submissions per queue?
- [x] fix resource viewer
	- [x] fix filtering by type
	- [x] fix filtering by name
	- [ ] more useful names for handles (e.g. some basic information for images)
- [ ] make sure to always correctly store/forward pNext chains
	  easier future compat, will support (non-crash) a lot of
	  extensions naturally already.
	- [ ] vkQueuePresentKHR is problematic atm
	- [ ] everywhere where we hook-create handles
	  nvm, we likely cannot/shouldn't do it without deciding on per-extension
	      basis. Just forwarding random pNexts will likely not work.
- [ ] implement overview as in node 1652
	- [ ] associate CommandGroup with swapchain (and the other way around?)
	- [ ] allow something like "update from swapchain" in command buffer viewer?
	      It seems to me we want a more general "command source" concept
		  for the command buffer viewer. Could be queue/command buffer/command group/
		  swapchain/identified per-frame submission/fence-association or 
		  something like that.
- [ ] implement additional command buffer viewer mode: per-frame-commands
      basically shows all commands submitted to queue between two present calls.
	  similar to renderdoc
	- [ ] or just show the most important submission for now? (based on "main
	      submission" heuristics)
- [x] rework dev/gui so that there is never more than one gui. Supporting
      multiple guis at the same time is not worth the trouble (think
	  about command buffer hooking from multiple cb viewers...)
	- [ ] what to do when window *and* overlay is created? or multiple overlays?
		  Should probably close the previous one (move gui object)
		  See todo in Gui::init
- [ ] Implement missing resource overview UIs
- [ ] Remove virtual stuff from this whole CommandBufferHook 
- [ ] Add more useful overview. 
	- [x] Maybe directly link to last submitted command buffers?
	      {this is kinda shitty though, need the concept of command buffer groups
		   to make this beautiful}
	- [ ] show graph of frame timings (see swapchain)
	- [ ] show enabled extensions & features
	- [ ] only show application info if filled out by app. collapse by default?
- [ ] next ui sync rework
	- [x] don't lock device mutex while waiting for fences (see waitForSubmissions)
	- [x] use chain semaphores for input
	- [ ] correctly sync output, but only if it's needed (might work already)
	- [x] if timeline semaphores are available, use them! for all submissions (in and out)
		- [x] when timeline semaphore extension is available, enable it!
	- [ ] when a resource is only read by us we don't have to make future
	      submissions that also only read it wait.
		- [ ] requires us to track (in CommandRecord::usedX i guess) whether
		      a resource might be written by cb
	- [x] insert barrier at end of each submission, optimize case where application
	      uses same queue as gui
- [ ] proper shipping and installing
	- [x] make the json file a config file, generated by meson
	- [ ] write wiki post on how to build/install/use it
	- [ ] fix for api: should probably load *real* name of library, not some guesswork.
	      Important on windows, to support all compilers.
		  See TODO on lib name there
- [ ] write small wiki documentation post on how to use API
	- [ ] could explain why it's needed in the first place. Maybe someone
	      comes up with a clever idea for the hooked-input-feedback problem?
	- [x] write something on instance extensions for compute-only applications
	      see: https://github.com/KhronosGroup/Vulkan-Loader/issues/51
		  {see docs/compute-only.md}
- [ ] improve window creation: try to match up used swa backend with enabled
	  vulkan extensions. Could extend swa for it, if useful
	  (e.g. swa_display_create_for_vk_extensions)
	- [ ] possibly fall back to xlib surface creation in swa
	- [ ] at least make sure it does not crash for features we don't
	      implement yet (such as sparse binding)
		   (could for instance test what happens when memory field of a buffer/image
		   is not set).
- [ ] improve buffer viewer
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
	- [ ] show texel color? (requires us to download texels, just like we 
	      do with buffers)
	- [ ] better display (or completely hide?) swapchain images
	      We should probably fill-in Image::ci for them.
- [x] fix "unimplemented descriptor category" bug (not sure when it appears)
      {we were casting from descriptor type to descriptor category in stead
	   of using the function...}
- [ ] automatically update resource lists in resource gui when tab is re-entered
      from somewhere else
- [ ] when we select a resource of type X should we set the current filter to X
      in the resource gui?
- [ ] imgui styling
	- [ ] use custom font
	- [ ] go at least a bit away from the default imgui style
	      The grey is hella ugly, make black instead.
		  Use custom accent color. Configurable?
	- [ ] Figure out transparency. Make it a setting?
	- [ ] see imgui style examples in imgui releases
- [ ] improve handling of transprent images
- [ ] probably rather important to have a clear documentation on supported
      feature set, extensions and so on
	- [ ] clearly document (maybe already in README?) that the layer
	      can crash when extensions it does not know about/does not support
		  are being used.
	- [ ] update README to correctly show all current features
- [x] support for buffer views (and other handles) in UI
	- [ ] use buffer view information to infer layout in buffer viewer?
	- [ ] support buffer views in our texture viewer (i.e. show their content)
- [ ] take VkPhysicalDeviceLimits::timestampComputeAndGraphics into account
	  for inserting query commands (check for the queue family in general,
	  we might not be able to use the query pool!).
- [ ] limit device mutex lock by ui/overlay/window as much as possible.
    - [ ] We might have to manually throttle frame rate for window
- [ ] allow to force overlay via environment variable. Even with go-through
      input (we might be able to just disable input for the whole window
	  while overlay is active using platform-specific stuff), this might
	  be useful in some cases, the extra window can be painful.
	- [ ] generally expose own window creation and force-overlay via env vars
- [ ] add example image to readme (with real-world application if possible)
- [ ] move external source into extra folder
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
- [ ] stop this todo-for-v0.1-list from growing at some point.
- [ ] before release: test on windows & linux, on all owned hardware


not sure if viable for first version but should be goal:
- [x] stress test using a real vulkan-based game. Test e.g. with doom eternal
- [ ] get it to run without significant (slight (like couple of percent) increase 
	  of frame timings even with layer in release mode is ok) overhead
	- [ ] vkQuake2
	- [ ] doom eternal
	- [ ] dota 2 (linux)

Possibly for later, new features/ideas:
- [ ] use new imgui tables api where useful
- [ ] should support image-less framebuffer extension as soon as possible,
      might need a lot of changes
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
- [x] bump api version as far as possible when creating instance?
      not sure if anything could go wrong in practice
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
- [ ] add example images to docs
- [ ] in draw/dispatch commands: we might have to check that layout of bound
      descriptors matches layout of bound pipeline
- [ ] general buffer reading mechanism for UI. Implement e.g. to read
      indirect command from buffer and display in command UI
- [ ] allow to display stuff (e.g. images) over swapchain, fullscreen, not just in overlay
- [ ] memory budget overview
	- [ ] show how much memory was allocated overall, per-heap
	      make this visually appealing, easy-to-grasp. Maybe via pie-chart or something.
		  We can probably start using ImPlot.
		  Maybe allow to have a global pie chart (showing *all* memory) and
		  then per heap/per memory type flag (allowing us to easily visualize
		  e.g. the amount of on-device memory allocated/available).
	- [ ] Also allow to color memory depending on the type it is allocated for.
	- [ ] allow to visualize by allocation size, i.e. showing the big memory blockers
	      (but also allow showing smallest first, can be useful)
	- [ ] visualize totally available memory as well, we can get that from
	      memory properties
	- [ ] there are extensions for querying the real allocated/free memory
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
- [ ] related to command buffer groups: simply view all commands pushed
      to a queue?
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
- [ ] interactive 3D model viewer
- [x] insert command buffer timing queries
- [ ] per-drawcall image visualization using the inserted subpass + 
      input attachment shader copy idea, if possible
- [ ] event log showing all queue submits
	- [ ] optionally show resource creations/destructions there as well
- [ ] resource and queue freezing
	- [ ] something like "freeze on next frame/next submission"
- [x] track query pools
- [x] track buffer views
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
	  But otoh dynamic_cast and hierachy is probably better for maintainability
- [ ] explore what random stuff we are able to do
	- [ ] Visualize models (drawcalls) on its own by inferring
	  	  position (and possibly other attribs; hard to infer though, could use heuristics
	  	  but should probably let user just flag them explicitly)
	- [ ] Infer projection and view matrix, allow to manipulate them.
	      We could add our entirely own camera to any game, allowing free movement
		  in the world (likely glitched due to culling and stuff but that's still interesting).
		  Hard to infer the correct matrix, might rely on manual user flagging.
	- [ ] Infer as much general information as possible. When annotations are
		  missing automatically annotate handles and the command buffer
		  as good as possible. We are likely able to detect depth-only (should probably
		  even be able to develop good heuristics to decide shadow vs preZ), gbuffer,
		  shading, post-processing passes. Might also be able to automatically infer
		  normal/diffuse/other pbr maps (harder though).
	- [ ] use heuristics to identify interesting constants in ubo/pcr/shader itself
		  (interesting as in: big effect on the output). Expose them as parameters
		  in the gui.
- [ ] include copy regions in argumentsDesc of transfer commands?
      would probably make sense but they should not be weighted too much
- [ ] (low prio, experiment) allow to visualize buffers as images where it makes sense 
	  (using a bufferView or buffer-to-image copy)
- [ ] (low prio) can we support android?
- [ ] (low prio, evaluate idea) allow to temporarily "freeze destruction", causing handles to be
      moved to per-handle, per-device "destroyedX" maps/vectors.
	  The vulkan handles probably need to be destroyed (keeping them alive
	  has other problems, e.g. giving memory back to pools, don't wanna
	  hook all that) but it might be useful to inspect command buffers without
	  handles being destroyed
- [ ] (low prio, non-problem) when using timeline semaphores, check for wrap-around?
      would be undefined behavior. But I can't imagine a case in which
	  we reuse a semaphore 2^64 times tbh. An application would have to
	  run millions of years with >1000 submissions per second.
	  Probably more error prone to check for this in the first place.
- [ ] (low prio), optimization: in `Draw`, we don't need presentSemaphore when
	  we have timeline semaphores, can simply use futureSemaphore for
	  present as well

