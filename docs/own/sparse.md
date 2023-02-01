# viewing sparsely bound images.

oh boi.

so for sparse residency it's fairly simple... I guess?
No wait, what about device memory that gets destroyed (and thus
	implicitly unbound while we render the image)?
	Hm, could track that via draw.usedHandles.
	But then what about memory ranges that get *explicitly* unbound?
	That is allowed right? right.
Ok, so just as hard as opaque bindings, just a bit different.

For opaque bindings the obvious difficulty is that we are only allowed
to display the image if it's fully bound to memory. And that that can
change any time.

But wait, it changes via queue submissions. That we can simply sync with.

So, when displaying a sparsely bound image:
- sync with all bindSparse queue ops that affect it
  (in both directions!)
- for not-sparse-residency: before displaying it, check whether everything
  is bound to memory. Check is inherently racy if we want to signal it via UI.
- for all: track device memory bound to image used in draw.
  when memory is destroyed while draw is in flight, wait for draw first.
  This means we need a callback into the gui every time a device memory
  object is destroyed. fair enough.
  BUT: gathering the device memories bound at draw time is again racy,
  must do it during final critical section of gui submission, 
  so not in ImageViewer itself.

How to approach race problems in point 2 and 3?
In the final critical section for submission, check if a drawn image
is sparse.
If so:
- for non-sparse-residency, check if it's completely bound.
  If not, ??? try again? and make the same check in imageviewer?
  But we might run into a loop... Maybe then just set special
  flag in gui and force imageViewer to show for this frame
  "not completely bound to memory"
  We really want to avoid a loop
- for all (AFTER non-sparse-residency check): gather the device memory
  objects it references and add them to the list of used handles for the draw.
  When device memory is destroyed, it'll notify, we can wait for the draw
  to complete. Easy.

---

How to handle it in command hook?
Just because an image is used *statically* by a pipeline we can't
assume we can copy it (I think? spec not 100% clear on this but
reads like this).
Ok so currently we hold the lock during recording anyways.
That is terrible but whatyagonnado.
Using that, we can simply check during recording and then associate
the (hooked) submission with the device memory objects it depends upon
that *may* be destroyed by the applicaiton. We then just wait
on the submission inside the layer first.
And obviously we then have to sync our hooked submission with all sparse
binding operations affecting the resource (past and future).

----

The spec seems to read like this: copying sparse resources is always ok,
even if only sparsely bound. *might* return undefined values, depending
on a sparse format property.
And the "handle/device memory is destroyed before our copying in hooked
submission finishes" thing is a general problem. Should be fixed in the 
next round of copy-cow-sync fixes/improvements. Hard to do again for
sparse stuff tho...

Wait, no, the spec states "It is important to note that freeing
a VkDeviceMemory object with vkFreeMemory will not cause resources (or resource
regions) bound to the memory object to become unbound. Applications must not
access resources bound to memory that has been freed".
That is super important. It means that we don't have to care that much about
destruction, we only have to make sure we probably sync with all binds.
Oh no it does not, nevermind. An application might have an image bound
but not *access* it, in which case this would be valid. Dammit.

So (in addition to proper gpu-level sync for binds/submissions), when a memory 
object is freed, we have to make sure all hooked submissions that use 
resources bound to it, have completed.
