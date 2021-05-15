Environment variables
=====================

- `VIL_CREATE_WINDOW={0, 1}`, whether to create a separate window with the vil gui
- `VIL_HOOK_OVERLAY={0, 1}` whether to hook the window input and render
  the overlay directly into the backbuffer, on top of the application.
  This mode is experimental, unstable and has problems. Also not supported
  on all platforms. Your mileage may vary.

- `VIL_WRAP={0, 1}` whether to enable object wrapping in general.
  If set to 0, will disable object wrapping for all handle types.
  This has an impact on performance (mainly for multi-threaded applications)
  but increases the chance that vil works with extensions it does not
  explicitly support.
- ` VIL_WRAP_{HandleType}={0, 1}` more fine-granular handle wrapping 
  configuration for each handle type. Can be used to minimize performance impact
  for non-wrapped handles that an application needs. 
  Only has an effect for handle types that have support for wrapping in vil
  in the first place. For instance,
  	- `VIL_WRAP_COMMAND_BUFFER` 
	- `VIL_WRAP_DESCRIPTOR_SET`
	- `VIL_WRAP_IMAGE_VIEW`
	- `VIL_WRAP_BUFFER`

- `VIL_TIMELINE_SEMAPHORES={0, 1}` whether to use timeline semaphores, if available.
  Using them can increase performance. In the past, it was still useful to
  be able to to disable them, e.g. for bugs in driver or validation layer.
- `VIL_TRANSFORM_FEEDBACK={0, 1}` whether vil should use transform feedback,
  if available. Could cause problems in some cases but without this, viewing
  the data coming out of vertex shaders won't be available.
