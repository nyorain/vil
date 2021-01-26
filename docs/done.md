For v0.1

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
