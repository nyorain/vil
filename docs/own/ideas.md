Moved from todo.md. Mostly ideas for experiments that might not even
be possible or useful in the end.

- serialize command records. At least roughly, without referenced handles
  and stuff
  	- beside visualization, this would make debugging easier, allowing
	  to use specific (complex) records in the unit tests for reproduction
	  and debugging

- allow to visualize buffers as images where it makes sense 
  (using a bufferView or buffer-to-image copy)
- allow to temporarily "freeze destruction", causing handles to be
  moved to per-handle, per-device "destroyedX" maps/vectors.
  The vulkan handles probably need to be destroyed (keeping them alive
  has other problems, e.g. giving memory back to pools, don't wanna
  hook all that) but it might be useful to inspect command buffers without
  handles being destroyed

- use dynamic rendering as renderpass splitting fallback?
  we'd have to recreate the pipeline as well tho. And can't support
  input attachments. Hm ok probably wont work
  	- Could do it on pipeline creation time when no resolve/input
	  attachments are used? But that might have performance impact
	- The renderpass splitting approach is hacky, error-prone, hard
	  to debug. So finding an alternative here (and dynamic rendering
	  sounds promising) would make sense and be worth it.

- add option to automatically write some specific pattern into images/buffers
  on initialization, making it easier to detect uninitialized memory usage

- explore what random stuff we are able to do
	- Visualize models (drawcalls) on its own by inferring
	  position (and possibly other attribs; hard to infer though, could use heuristics
	  but should probably let user just flag them explicitly)
	- Infer projection and view matrix, allow to manipulate them.
	  We could add our entirely own camera to any game, allowing free movement
	  in the world (likely glitched due to culling and stuff but that's still interesting).
	  Hard to infer the correct matrix, might rely on manual user flagging.
	- Infer as much general information as possible. When annotations are
	  missing automatically annotate handles and the command buffer
	  as good as possible. We are likely able to detect depth-only (should probably
	  even be able to develop good heuristics to decide shadow vs preZ), gbuffer,
	  shading, post-processing passes. Might also be able to automatically infer
	  normal/diffuse/other pbr maps (harder though).
	- use heuristics to identify interesting constants in ubo/pcr/shader itself
	  (interesting as in: big effect on the output). Expose them as parameters
	  in the gui.

- Allow modifying resources (temporarily or permanently)
	- in command viewer or resource viewer
	- over such a mechanism we could implement a forced camera

- optimization: instead of copying resources in commandHook, implement
  copy-on-write. Only when this submission (or some other pending one)
  is potentially writing something, copy it. Otherwise, copy it before
  future submissions that might write it (if still needed)

- add submission log! possibility to track what submissions are done
  during startup, another thing that's hard to track with capturing
  we just create a second window by default?
  If a manual overlay is created later on, we can still close the window
  and move the gui. Could also wait for first submission/swapchain
  creation (if swapchain ext is enabled) with creating/showing the window

- add anti-aliasing to vertex viewer. Should probably be beautifully
  be doable with TAA
  	- could also add AO to vertex viewer, when rendering faces

- add a "force render-order" feature since it can be a pain in the ass
  to continuously debug draw commands when their order randomly changes
  (have seen it multiple times in games). Not sure if feasible though to
  force/reorder on our side.

- allow to show contents of *any* resource before/after *any* command.
  Useful debugging memory corruptions & aliasing.

- allow to save image/buffer data to file

- time critical functions: Gui::Draw, command hook recording, freeBlocks etc
  and track them via DebugStats. When they get too large show something
  via imgui in debug mode (e.g. the needed time in red).

- hide shader debugger behind config feature?
	- adds a whole new library and isn't ready yet at all.
	  and might never really be ready, it's more of an experiment anyways
