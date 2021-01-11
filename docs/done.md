For v0.1

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
