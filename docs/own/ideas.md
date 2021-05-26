Moved from todo.md. Mostly ideas for experiments that might not even
be possible or useful in the end.

- allow to visualize buffers as images where it makes sense 
  (using a bufferView or buffer-to-image copy)
- allow to temporarily "freeze destruction", causing handles to be
  moved to per-handle, per-device "destroyedX" maps/vectors.
  The vulkan handles probably need to be destroyed (keeping them alive
  has other problems, e.g. giving memory back to pools, don't wanna
  hook all that) but it might be useful to inspect command buffers without
  handles being destroyed

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

- add anti-aliasing to vertex viewer. Should probably be beautifully
  be doable with TAA
  	- could also add AO to vertex viewer, when rendering faces
