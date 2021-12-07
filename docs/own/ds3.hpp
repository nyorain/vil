// See node 2063

struct DescriptorSet;
struct DescriptorSetLayout;

struct DescriptorPool {
	VkDeviceSize dataSize;
	VkDeviceSize offset;
	std::unique_ptr<std::byte[]> data;

	struct Entry {
		VkDeviceSize offset;
		VkDeviceSize size;
		DescriptorSet* ds;
	};

	std::vector<Entry> entries;
};

struct DescriptorStateCopy {
	u32 variableDescriptorCount {};
	u32 _pad {};
	// std::byte data[];
};

// When a DescriptorSet with a pending cow is destroyed or written,
// it will resolve it first, i.e. create a copy of its state in the
// cow object. Aftewards, it will disconnect itself from the cow.
// DescriptorCows are ref-counted (allowing multiple independent sources
// to reference the state of a descriptor state). When the reference
// count is decreased to zero and the cow destroyed, it will disconnect
// itself from the DescriptorSet.
struct DescriptorCow {
	DescriptorSet* ds {};
	DescriptorStateCopy* data {};
	u32 refCount {};
};

struct DescriptorSet {
	std::byte* data {};
	IntrusivePtr<DescriptorSetLayout> layout {};
	u32 variableDescriptorCount {};
	DescriptorCow* cow {};
};

// Stored in CompletedHook
struct CommandDescriptorSnapshot {
	std::unordered_map<void*, IntrusivePtr<DescriptorCow>> states;
};

//
struct Device {
	// ...
	// Look mutex before accessing.
	// Memory for copies allocated from some memory block manager.
	std::vector<IntrusivePtr<DescriptorStateSnapshot>> dsSnapshotPool;
};

// Ideally, we wouldn't have anything to do in ResetDescriptorPool.
// What if we don't keep written handles alive via intrusive ptr?
// Do we *really* need to know if they get destroyed?
// Well yeah, kinda.

// otherwise, viewing an invalidated record won't work.
// otherwise, matching with an invalidated record won't work.
// damn.
// If that was *really* the only problem, we could at least
// move it to another thread. Making sure it doesn't block the application.
// And most applications don't use full multithreading potential.
// But what about that ton of other stuff we do in destroy(ds)?
//
// WAIT A MINUTE.
// Do we only really need the intrusive pointers in copied states maybe?
// Hm we might have a cow on a ds and then an imageView in it gets
// destroyed, that does not help.
// We kinda need the intrusive pointer as soon as we add the cow. Only
// then will we access it later on. So maybe manually increase/decrease
// the refCounts in those situations.
// - in addCow, iterate over set and increase all ref counts
// - in copyLockedState, basically take ownership of the reference.
// - in ~DescriptorCow, if no copy was made, iterate over set again
//   and decrease ref counts. If copy was made, iterate over copies
//   and decrease ref counts for them.
//
// That still means we can't view ds state in gui. Kinda sucks.
// Maybe make it environment variable? For short lived descriptor sets,
// that feature is useless anyways.
// env: VIL_VIEWABLE_DS. Set to 0 by default.
// (We might still make it possible to view the valid bindings in gui even
//  if it's 0. Just look up in the maps if the handle* is known. If so,
//  it's safe to link. Might get a new (reallocated) handle tho, but
//  it's better than nothing. Maybe screw the env variable and
//  default to this behavior? Hm nah, it's kinda misleading. But making
//  it default sounds ok. As long as keeping the two paths is
//  straight forward but I expect it to be).
//
// And suddenly ResetDescriptorPool is nothing more than some
// checkResolveCow calls pretty much (yeah, we still need to figure
// out proper disconnection from CommandRecord).

// ---

// we can't just make a ds snapshot in gui/cb.cpp with refBindings false. Bindings
// might be invalid, would crash when increasing ref count.
//
// Solution: check for each binding if it's known. To avoid false positives:
// - either have a limited-size list of destroyed handles in the device. And then
//   avoid recreating at same address or something. Ugh.
//   - maybe we can come up with some clever allocation scheme for
// 	descriptor-relevant handles that manages this automatically? Like, it
// 	minimizes the chance that this happens by offsetting allocations or not
// 	reusing them or something.
// - Only in this case: Store the time when the selected record was submitted. And
//   handle creation time. If handle creation time was later, reject it. Ugh. This
//   wouldn't even catch all cases with update_unused_while_pending i guess. UGH
// - Just don't give a fuck an show false positives? chance shouldn't be too high.
//   And we could just show "Destroyed" otherwise. Which would at least be less
//   confusion tho
// - store ids in the bindings, allowing to reject later on as we do with
//   descriptor sets. Massive increase of memory needed by descriptors, almost
//   doubled. UGH
//
//
// WAIT we can add a spin to the first approach, making it almost ok. Just keep
// the handles itself, only marking them as destoyed. And then only clean if it
// gets too large. Then we can even show correct information. And it's a nice
// balance between memory overhead and time of keeping alive (could even be
// runtime tuned via setting).
//
// To make 100% sure you never get false positives, just enable refBindings.
