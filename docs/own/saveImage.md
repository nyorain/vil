Saving image to file from gui
=============================

We have `ImageViewer::saveToFile` but it's racy, not properly synced,
blocking, and in general just terrible.
Instead:
- properly sync
- don't do own submission to queue if possible, just use gui submission for copy?
- do all heavy cpu work (writing to file via imgio) using an async job

We don't need a lot of differentiation if it's a HookState image or application
image, really. We just:
- Initiate the operation when the buffer is pressed.
	- Allocate host buffer that is large enough
	- add pre/post record callback or local flag that the image should be copied
	- Add draw completion callback that starts a thread, moving ownership of
	  the buffer into it
	- The used image should already be in usedHandles if it's an application
	  image.
- saving thread: 
	- just write it to file using imgio
	- free (implicitly by out-of-scope) the host visible buffer

Not sure:
- how to handle sparse images. Does vkCmdCopyImageToBuffer is well defined
  here?
  	- wait, need to think about copying those resources in CommandHook
	  as well.
	  Hm, not too much. See VkPhysicalDeviceSparseProperties::residencyNonResidentStrict:
	  Reading is safe in any case and if this is set, it's even
	  guaranteed to return 0. Nice.
