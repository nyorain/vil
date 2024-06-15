# AccelStructState rework

```cpp
struct AccelStructBuildOp {
	CommandRecord* record {};
	IntrusivePtr<AccelStructState> dstState {};
	// ...
};

struct AccelStructState {
	// if >0, must not be modified
	// synced via device mutex.
	// TODO: not sure if this can work.
	//  need some kind of ref count?
	//  implement full proper cow logic?
	u32 numReaders {};

	AccelStruct* accelStruct {};
	std::variant<AccelTriangles, AccelAABBs, AccelInstances> data;

	// Built hardcoded into a cb that can still be submitted.
	// In that case we can't have readers, they need to take a copy.
	bool builtStatically {};
};

struct AccelStructStateRef {
	AccelStructState* state;

	AccelStructStateRef(AccelStructState& state); // increases numReaders
	~AccelStructStateRef(); // decreases numReaders
};

struct AccelStruct : SharedDeviceHandle {
	// ...
	IntrusivePtr<AccelStructState> state;
}

// make AccelStructStateRef part of CommandHookState::CopiedDescriptor
```

On recording:

```
CmdBuildAccelerationStructure(params) {
	if(cb.capture) {
		// Normal recording, add as command.
		// Will be resolved during hooking
		// ...
	} else {
		auto& build = record.accelStructBuilds.emplace_back();
		build.accelStruct = ...
		// only need to create new state if buffers aren't large
		// enough anymore.
		// Otherwise use existing accelStruct state.
		build.dstState = ...
		build.dstState->builtStatically = true;

		// records the commands needed to capture the data
		// directly into this cb
		recordCaptureAccelStructData(cb, build.dstDate, params);

		dispatch.CmdBuildAccelerationStructure(params);
	}
}
```

On submission activate:

```cpp
for(auto& build : record.accelStructBuilds) {
	build.accelStruct->state = build.dstState;
}
```

On hook record:

in copyDs, get the current state for the AccelStruct (not so trivial,
might have changed in the current record but we can't update
AccelStruct.state until we know the submission is active).
If that state has `builtStatically = true` we can't use it :(
Ok, so I guess now we see that we need a proper CoW mechanism.
let's try again

```
using AccelStructData = std::variant<AccelTriangles, AccelAABBs, AccelInstances>;

struct AccelStructState {
	std::atomic<u32> refCount {};

	// synced via device mutex.
	// TODO: does this ever change though? Should stay the same on cpu side
	AccelStructData data;

	// cow on the current state.
	// synced via device mutex.
	IntrusivePtr<AccelStructCow> cow {};

	// remember last submission that updated it?
};

struct AccelStructCow {
	// Mutex protects accelStruct and copy. Needed since accessing the cow
	// and resolving it may happen in parallel from multiple threads.
	DebugMutex mutex;

	// only set when it wasn't changed/destroyed since CoW was constructed
	AccelStructState* accelStruct {};

	// only set when copy already happened
	std::unique_ptr<AccelStructData> copy {};

	std::atomic<u32> refCount {};
};

AccelStruct {
	IntrusivePtr<AccelStructState> state {};
};

struct AccelStructBuildOp {
	CommandRecord* record {};
	AccelStruct* accelStruct {};
	IntrusivePtr<AccelStructState> dstState {};
	// ...
};
```

- copyDs can just store a AccelStructCow
	- hmmm no I think we can only do this once the respective submission
	  is activated right?
	  so copyDs just stores a cowToBeActivated (or some general activateCallback)
	  inside the submission/hookRecord that is fired up on activation.
- when a submission that updates AccelStructState (either dynamically
  or statically) is submitted (activated? don't think so. Just need to carefully
  sync it with potential later submissions that might be executed first),
  it first checks if it needs to resolve the cow.
  If so, it first submits the copy command.
  (wait, but at that point we can't submit it anymore before the activated
  submission... oh wait yes we might, using timeline semaphores I guess?
  but not so sure tbh)
- we still need special handling inside of command hooking.
  if the state we'd want to update already has a cow, just create a new state?
  Or resolve it inside the hooked record? not sure if we want that tho...
  creating a new state sounds like the better solution.

This whole async submission/order thing is making this really hard :(
If we already know at copyDs time that the state is written in future (by
a submission that was not activated yet)(need an extra flag or something)
just copy the state instantly and don't bother with the whole cow thing
(important since it won't be resolved correctly otherwise).

we REALLY don't want to delay hooking/submission until a submission is
activated. That is way too intrusive.

---

# Next iteration 2024

- meh, don't do any cow stuff for now. Keep it simple.
- hard case: when viewing a tlas we don't want to blases (or its state)
  to be destroyed. Could capture it once.
	- current bad case: TLAS is build. BLAS referenced in it is destroyed.
	  TLAS state is viewed in gui. :(
	- capturing would happen in CommandHookSubmission finishing.
	  Should be enough. But how to capture it? maybe store
	  "finishedState" in accelStructs? hm when blas is built AFTER
	  tlas was build (e.g. in same cb), it's wrong again.
	  At each tlas build, capture a full mapping of all blas
	  addresses to their current states (at that point in time).
	  This could be very expensive :/
	- fully correct way: maintain some kind of mapping on gpu.
	  data structure allows getting metadata for blas address.
	  we then write out the version number of the blas at the time
	  of building, when copying tlas instances.
	  uff. Then only need to ensure blas states are still alive when
	  resolved (for ref count inc).
	  Complicated!

Hm okay wait. When a tlas is viewed in resource viewer, these
considerations do not matter: when a blas in it was destroyed/updated/rebuilt
afterwards, its invalid anyways. We just show new blas state in ui, no
big deal.
So its only important when viewing tlas in dispatchRays viewer. Or
for shader debugging or something. But when we capture the tlas
via descriptor, we could just create a map of active blas states
at *that* point. `UnorderedMap<u64, IntrusivePtr<AccelStructState>>`

In resource viewer, can compare TLAS build time with BLASes build times
and show "invalid; blas out of date" warning.
When a submission with a TLAS is used, we can be sure that is is valid
*at that point*. So just capture the TLAS and all BLAS states at that point.
