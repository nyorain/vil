What to do when we display a command buffer that is currently recorded
in another thread? for now we simply display the incomplete cb.

Instead, we might wanna display the previous cb state. Or at least mark
it as not finished.

---

On synchronization and resource lifetimes:

- the application might destroy a vulkan object while we are currently
  displaying information about it in the overlay renderer. How to
  handle that?
  	- first try was to require handle destructors to be called while 
	  the device mutex is locked (we already need to do that for
	  removing it from the device's resource list) and informing
	  all renderers about it

But the problem isn't restricted to renderers. Any handle may reference
another handle. And often enough vulkan allows to destroy handle A even
though B still references it, e.g. you can destroy an image before a
framebuffer it is used in (couldn't find anything in vulkan saying it's not
allowed at least, validation layers don't warn; even though that obviously 
makes the framebuffer invalid and you can only destroy it afterwards). 
Especially for command buffers (where we might want to display the state
even if they are invalid by a referenced resource being destroyed/changed)
this is a problem.

Second try:
Just use shared pointers where resources are referenced. Less critical sections,
keeping our resource information alive for debugging purposes is *exactly* what we want.
Example:
- In a Framebuffer, we store its attachment at shared_ptr<Image> instead of just Image*
- In Image, we want to know all framebuffers it's used in as well. But we are not
  interested in the framebuffers it was previously used in. That's why we simply
  use Framebuffer* here.
This means, when destroying a framebuffer we inform all associated images that
the framebuffer was destroy (which is natural since framebuffer builds on top
of image) but when an image is destroyed, we never have to inform any
framebuffers (which was weird to do in the first place).
