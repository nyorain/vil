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
