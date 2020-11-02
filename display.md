# Display

There are different methods for creating a debugging display.

## Overlay

Usually, layers draw directly onto the window they hook themselves
onto (e.g. renderdoc) but this has a big problem: Input.
On most platforms there are ways to hook our layer into the windows'
input as well but sometimes this comes with restrictions such as
not being able to suppress the input being sent on to the application.
Which can make interacting with the overlay awkward.
Furthermore, this method requires a window in the first place. This means
we can't show any debug information for compute-only applications.

The hooking works like this:

- On swapchain creation, we retrieve the images from the swapchain and create
  our own views and framebuffers. We also create our own renderpass, pipeline and
  other render data for the swapchain (as well as an imgui context).
  We can't really share these objects between multiple windows
  since they might have different formats (requiring different render passes)
  and doing so might require some more synchronization.
- When the application calls vkQueuePresentKHR on the window, we simply
  submit our own command batch (which first waits upon all work that
  vkQueuePresentKHR was supposed to wait on) and then call vkQueuePresentKHR
  down the chain, making it wait upon our overlay rendering via a new semaphore.

## Separate window

The second option (less common) is to simply create a new window from
within the layer. This requires some platform specific code as well,
but technically we can simply use an existing cross-platform toolkit for
window creation and input handling, making the layer a lot easier to manage.
This window could technically also be a child window to the hooked one
(if there is one). But we might also try to keep our own window as separate from
the application window as possible (making things like event dispatching
easier).

The difficulty here is that we want to use the application's device to present
to a window that was created inside the layer.

One way to hook is this (completely separate from application, working for
pure compute-applications as well):

- before instance creation, see which window backend is available and create
  a display instance for it
- enable all extensions we need to create a window on that backend in the
  created instance, if they were not already
- create a window and surface for it
- on device creation, we just have to pray that the given device supports
  the given surface. Nothing we can do if not.
  We always create our own graphics and present queues for synchronization
  purposes.
  	- we then simply create our own thread running a main loop for our
	  debug window

## Separate process

Like renderdoc, we could also have a completely separate process show
the debug data. But this defeats the purpose of having a small in-app
debugging layer that is able to continuously give full introspection.
The problem with a separate process is that sharing device resources
(think of large images and buffers) can become a struggle, slow and
error-prone.
