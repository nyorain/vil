Copy on write optimization for CommandHook
==========================================

In CommandHook, we sometimes have to copy *huge* buffers and images.
But most of the time, that copy isn't actually needed since the application
doesn't modify the data anyways. So instead of always copying, we can
just implement a copy-on-write mechanism where possible.

Keep in mind:
- In some cases, we can't detect writes (mapped buffers/images,
  buffers with a DeviceAddress etc)
- We have to implemenet CoW conservatively, i.e. have to copy in all
  cases before the application *could* write something (e.g. storage buffer).

```
struct CowImage {
	std::optional<CopiedImage> own;

	Image* source;
	VkImageSubresourceRange range;
};

struct CowBuffer {
	std::optional<OwnBuffer> own;

	Buffer* source;
	VkDeviceSize offset;
	VkDeviceSize size;
};

// In image (buffer analogously)
{
	....
	std::vector<CowImage*> cows;
};

// In CommandHookState, e.g. dsCopy
{
	std::unique_ptr<CowImage> image;
}
```

When a submission would modify a resource that has connected cows (or when
such a resource is destroyed), we simply
- first submit the copy operation
- disconnect the cow from the resource 'cows' vector and unset the source in it

We need special handling for resource destruction: We'd have to make sure to
keep the resource alive until our copy submission finishes.

### Timing & sync

What if the gui has a pending submission using an application resource
for drawing (cow saved us a copy) but then the application modifies
it:

- if it modifies it directly (e.g. MapMemory) we'd have to block in the
  call until our submission returns which is problematic. Maybe don't
  use the cow mechanism for mappable resources for now
- if it modifies it via a submission, we can simply chain that submission
  to our gui submission (via timeline semaphore).

---

What if the gui is *recording* it's submission while this happens?
We might have a race here.

Solution: We somehow have to signal *in the cow object* whether the
original object is *scheduled for use*. Just a semaphore isn't enough
tho since we might not have it/it might be signaled.
A timeline semaphore would probably solve it but create an ugly
dependency where we have to make an application submission wait
on our *cpu-side* gui rendering to finish by waiting on a semaphore/timepoint
with an associated submission that is still being built.

---

The operation resolving the cow needs to be synced with *all* operations
the application does involving the resource.
Actually, it's only that strict for images since we might have to
transition them.

The transform feedback problem
==============================

This won't solve our transform feedback problem, where we sometimes have
waaay too much data as well (causing significant slowdowns even on
high end gpus). The solution for that problem is paging. The problem
is that the vulkan xfb api does not allow to specify a capture
offset. So we'd have to manually split a too-large draw call into multiple
smaller ones with doesn't sound trivial. Especially for indirect multi draw.
Maybe just make split up multi draw into the individual draw calls and
implement paging on that base? We can then later on still investigate how to
split up single draw calls, we then have to do both anyways.

# Figuring out when a buffer/image is (potentially) written

We only want to add Cows on large objects anyways (or maybe when there
are many objects e.g. descriptorSet with >100 images). But for those
we need to figure out when they are written to correctly resolve the cow.
Multiple problems:

- For image variables in shaders, we know whether they are readonly or not,
  for buffers we don't. To make this work well with large storage buffers
  (think e.g. vertex/index buffers for raytracing) we just need to do
  our own shader analysis. Basically, just check each OpStore/OpImageStore
  instruction or whatever else may write to image/buffer from shader.
- When to check?
  During recording, we know which bindings are potentially written to.
  But that might bring quite some overhead for *all* records.
  Alternatively, we could only check during hook/submission time, in most
  cases there won't be any active cow and we can skip everything.
  Probably better to not do it during recording already.
- store a global list of all cow objects for this?

pseudo code in submission logic:

checkCow(submission)
	if there are any cows
		for usedHandle : submission.usedHandles
			if usedHandle.handle is descriptorSet
				for command : usedHandle.commands
					store bindings potentially written by pipeline
				for binding : bindings
					if binding is potentially written by any pipeline
						resolveCows(binding)
			else if usedHandle.handle is image
				if usedHandle.imageWritten
					resolveCows(usedHandle)
			else if usedHandle.handle is buffer
				if usedHandle.bufferWritten
					resolveCows(usedHandle)

The most costly one will be the descriptorSets.
But for most sets we can probably early-out, add fast path in
layout for sets that don't have any writable bindings to begin with?

__For now, let's just ignore storage image writes, it's too hard to track
efficiently. We just focus on resources (for now: only images) written
explicitly, i.e. via transfer. Add support for attachments later__

# Support mappable resources

in MapMemory:

```

	// resolve cows of mapped resources
	const auto mapEnd = mem.mapOffset + mem.mapSize;

	std::optional<CowResolveOp> cowResolve;
	auto getCowResolve = [&]() -> decltype(auto) {
		if(cowResolve) {
			return *cowResolve;
		}

		// create
		auto& cr = cowResolve.emplace();
		initLocked(dev, cr);
		return cr;
	};

	for(auto* alloc : mem.allocations) {
		if(alloc->allocationOffset + alloc->allocationSize <= offset) {
			// no overlap
			continue;
		}

		if(alloc->allocationOffset >= mapEnd) {
			// break here, allocations are sorted by offset
			break;
		}

		if(alloc->objectType == VK_OBJECT_TYPE_BUFFER) {
			auto& img = static_cast<Image&>(*alloc);
			for(auto& cow : img.cows) {
				recordResolve(dev, getCowResolve(), *cow);
			}
			img.cows.clear();
		} else if(alloc->objectType == VK_OBJECT_TYPE_IMAGE) {
			auto& buf = static_cast<Buffer&>(*alloc);
			for(auto& cow : buf.cows) {
				recordResolve(dev, getCowResolve(), *cow);
			}
			buf.cows.clear();
		} else {
			dlg_error("unreachable");
		}
	}

	if(cowResolve) {
		auto& cr = *cowResolve;
		++cr.queue->submissionCounter;

		VkSubmitInfo si {};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1u;
		si.pCommandBuffers = &cr.cb;

		// add full sync
		dlg_assert(dev.doFullSync);

		ThreadMemScope tms;
		auto maxWaitSemCount = dev.queues.size();
		auto waitSems = tms.alloc<VkSemaphore>(maxWaitSemCount);
		auto waitStages = tms.alloc<VkPipelineStageFlags>(maxWaitSemCount);
		auto waitVals = tms.alloc<u64>(maxWaitSemCount);
		auto waitSemCount = 0u;

		for(auto& pqueue : dev.queues) {
			auto& queue = *pqueue;
			if(&queue == cr.queue) {
				continue;
			}

			// check if pending submissions on this queue
			u64 finishedID;
			dev.dispatch.GetSemaphoreCounterValue(dev.handle,
				queue.submissionSemaphore, &finishedID);
			if(finishedID == queue.submissionCounter) {
				continue;
			}

			waitVals[waitSemCount] = queue.submissionCounter;
			waitSems[waitSemCount] = queue.submissionSemaphore;
			waitStages[waitSemCount] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			++waitSemCount;
		}

		si.pWaitDstStageMask = waitStages.data();
		si.pWaitSemaphores = waitSems.data();
		si.waitSemaphoreCount = waitSemCount;
		si.signalSemaphoreCount = 1u;
		si.pSignalSemaphores = &cr.queue->submissionSemaphore;

		VkTimelineSemaphoreSubmitInfo tsInfo {};
		tsInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		tsInfo.waitSemaphoreValueCount = waitSemCount;
		tsInfo.pWaitSemaphoreValues = waitVals.data();
		tsInfo.signalSemaphoreValueCount = 1u;
		tsInfo.pSignalSemaphoreValues = &cr.queue->submissionCounter;

		si.pNext = &tsInfo;

		auto res = dev.dispatch.QueueSubmit(cr.queue->handle, 1u, &si, cr.fence);
		dlg_assert(res == VK_SUCCESS);

		finishLocked(dev, cr);
	}
```

# Ordering and timeline semaphores

Timeline semaphores allow this:

1. submit batch2, waiting for timelineSemaphore ts==1, reading image A. Hooked.
2. submit batch1, writing image A, signaling ts=1

In this case, a trivial detection algorithm would add a cow resolve to batch1,
just because it is submitted later than batch2. But batch1 is actually
executed before batch2, therefore not needing the copy (and actually
making the copy incorrect as it would be added before the changes made
by batch1, which in turn would be visible by batch2 and the hooked
command tho).
How can our resolving algorithm detect this?
It really seems like we have to inspect the whole submission graph and
check if the batch is ready to execute? And only mark cows as 'active'
as soon as we detect the batch to be logically ready for execution (i.e.
we know all submissions happening before it). And not resolve cows
before they are 'active'.
But even then, we wouldn't catch all cases. We would incorrectly add
the resolve to the first submitted-out-of-order write submission.
So, in conclusion, we would have to postpone the cow adding (or activating)
and resolve op scheduling until we know a submission to be logically ready.

Wow, that complicates things a lot!
On the other hand, having such an application submission graph would be
useful anyways.

What about binary semaphores?
We know that submissions can't be out-of-order and yet chained,
that's not allowed. So we don't need that logic there. But maybe it's
easier to implement it independently of semaphore type.

----

And what about events?
In theory, an application could do the following

- submit workload A (that waits on Event E in beginning) on queue Q1
- submit workload B on queue Q2
- wait on host until workload B is finished, only then signal E

That creates an invisible dependency from B to A.
No idea if this is valid per spec. No idea if we can support this at all.
In the end we could still disable cows if events are used or something (or
based on an environment variable).

EDIT: yeah, seems valid per spec. damn.
EDIT EDIT: no, not really. See discussion in
	https://github.com/KhronosGroup/Vulkan-Docs/issues/755

---

ok, so let's do this. Submission rework, with full tracking and all that

```cpp
// sync.hpp
// For each semaphore, track all wait and signal ops currently pending
struct Semaphore {
	struct SyncOp {
		Submission* submission;
		u64 value {1u}; // always 1 for binaries
		VkPipelineStageFlags stages; // only when waiting
	};

	vector<SyncOp> signalOps;
	vector<SyncOp> waitOps;
};

// cow.hpp
struct Cow {
	// XXX: nope, see Submission::activate
	// bool active {false};
};

// queue.hpp
struct Submission {
	struct SyncOp {
		Semaphore* semaphore {};
		VkPipelineStageFlags stages; // only for waiting
		u64 value {1u}; // always 1 for binary sems
	};

	vector<SyncOp> waits;
	vector<SyncOp> signals;

	u64 lowerBound {};
	u64 upperBound {};

	vector<IntrusivePtr<Cow>> pendingCows;
};


// Submission::activate():
activate(subm) {
	for(auto& cow : subm.pendingCows) {
		// cow.active = true;

		// inserts the cow into its associated resource
		// NOTE: doing it like this means that if the resource is destroyed
		// before the submission is activated, we can't capture it.
		// But in that case we couldn't have captured it via copy
		// in the submission anyways??
		// Weird corner case, probably happens only for some
		// update_unused_while_pending stuff anyways (that we don't
		// fully support atm). Should just not support showing
		// unused bindings with update_unused_while_pending set,
		// sync for that is pretty much impossible.
		applyCow(std::move(cow));
	}
	subm.pendingCows.clear();

	for(auto& signal : subm.signals) {
		signal->semaphore->updateUpper(signal->value);
	}
}

checkActivate(subm) {
	auto submActive = true;
	for(wait : subm.waits) {
		if(wait.semaphore->isTimeline()) {
			if(wait.value > wait.semaphore->upperBound) {
				submActive = false;
				break;
			}
		}
	}

	if(submActive) {
		activate(sumb);
	}
}


// on submission:
checkActivate(subm);


// in checkLocked(SubmissionBatch&)
// when we know that it's finished
// we first made sure to logically process (checkLocked) all dependencies
for(auto& subm : batch.submissions) {
	for(auto& wait : subm.wait) {
		if(wait.semaphore->isBinary()) {
			// reset it
			wait.semaphore.lowerBound = 0u;
		} else {
			// TODO: not needed, right?
			// signal->semaphore->lowerBound = std::max(lowerBound, wait.value);
		}
	}

	for(auto& signal : subm.signal) {
		// just signals it for binary semaphores
		signal->semaphore->updateLower(signal.value);
	}
}

// vkSignalSemaphore also calls semaphore->updateUpper
// and I guess it also sets lowerBound?

Semaphore::updateUpper(u64 value) {
	if(this->upperBound > value) {
		return;
	}

	upperBound = value;
	for(auto& wait : waits) {
		// resolve the wait
		if(wait.value > value) {
			continue;
		}

		// check if this activated the submission
		if(!wait.submission.active) {
			checkActivate(submission);
		}
	}

	this->value = value;
}

// comment from later, during implementation:
// TODO: need more complicated logic for something like cows or lazy
// copying because of the stageMasks. In checkActivate, we must evaluate
// the max stage that could be executed now and then check for all pending
// ops whether something needs to be done.
// NOT only in activate
// ---
// EDIT: nvm, we have to modify the wait stageMasks anyways when the submission
// isn't activate on submission time.
```

side note:
- we can't create cows while recording the hooked record since it might
  be reused. Instead, we have to create them when using or re-using it.
  So each HookRecord basically stores a list of cow prototypes that
  are then created when its used.

Hm, but with this we can't really track everything yet.
Do we want a second data structure for that?
Hm, no let's try with submissions directly.

```
struct SubmissionGraph {
	// the batches (or rather, the submissions inside the batches) are our nodes.
	// and the wait/signal ops inside submission are our edges.
	// We just need to make sure semaphores aren't destroyed, use
	// IntrusivePtrs for that.
	using SyncedOp = std::variant<
		SwapchainAcquireOp,
		PresentOp,
		QueueWaitOp,
		IntrusivePtr<SubmissionBatch>,
		IntrusivePtr<BindSparseBatch>>

	std::vector<SyncedOp> ops;
};
```

So we need to make `SubmissionBatch` and `Semaphore` shared and should
be good? Pretty easy.

# Reflections: are Cows really worth it?

So, using cows will introduce additional complexity in vil.
Maybe this isn't the right way. Let's see.

Most images and buffers, we just *have* to copy, no way around it.
E.g. storage image/buffer, color attachment etc. They are often directly
modified again.

Where would we save performance with cows?
Static textures/buffers. E.g. material textures, vertex buffers.
BUT, do we really need cows for them? When we don't expect resources
to be modified, couldn't we simply just use them directly?
What in the case they DO get modified after all (e.g. a material
texture might be streamed out, a mesh swapped out for a higher LOD
or something)? Well, maybe just don't handle that case? Just wait
another frame? Or use the new content?
Let's explore if such approaches are viable.
__Detecting__ that a resource is potentially changed is easy, we don't
need the whole complexity of the CoW mechanism fore that.

Our main usecase for cows is the shader debugger. Secondary usecase:
sometimes inputs to shaders are HUGE static resources (like >100MB textures,
think highly detailed cubemap or something). Having some CoW mechanism
there would be nice as well.

Shader debugger:
- we run it for a retrieved HookState and let's say one of the input textures
  wasn't copied because we don't expect it to change (could be based on
  heuristics, e.g. if it wasn't changed the last 100 frames, it's likely
  it doesn't get changed now). But now we detected that it is indeed
  being modified. We could still "just use it" for debugging.
  This means we might get weird results for one frame. So what, nobody
  will notice :D
  On a more serious note, performance in the shader debugger is mainly a
  concern when not having frozen state. We could always force copies when
  our state is frozen so we guarantee coherent input state in that case.
	- alternatively, if we detect the change, we could simply show some
	  message in the UI. Or maybe both, "just use it" (TM) and show
	  a little warning signaling that stuff was unexpectedly changed.
	  Good thing: if the changed resource isn't even accessed from the
	  shader, everything is alright (as will often be the case for
	  the huge bindless table case that we need this for).
- what in the case that it was destroyed? just sample it as 0 and show
  the warning I guess.
So, in summary, "just use it" sounds quite viable for the shader
debugger.

Resource viewer:
- Similar to the shader debugger case. When state is frozen, we can afford
  a copy (once). Otherwise, during continuous viewing, having a potentially
  changed resource here shouldn't be a big deal.
  Might be a good idea to allow forcing copies via a setting

---

Just noticed something: we can always "afford one copy", it's just this
continuous copying that's an issue. But for resources that are rarely
changed, one copy is enough most of the time. So instead of CoW, maybe
we can just re-use copies? Should be a lot easier.

Hm, this does NOT solve our problem with huge bindless tables (e.g. holding 16k
material images), we can't afford a single copy on the whole table EVER.
Maybe use the approach from above for them.

Also, all of this - even just detecting modification - has similar sync
problems to the cow case. Out-of-order submissions using timeline semaphores
makes this hard. So in either way, we need submission tracking.

OTOH, when just using state "as is" we don't care about those details
too much, could show the warning (or skip using it) in every frame
where it was changed.

So, should we implement sync tracking as outlined above?
Just noticed: we need it anyways, full sync is (currently) broken without it.
And since we want/need full sync anyways, let's implement sync tracking.
Might also be useful in UI
