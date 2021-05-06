#include <fwd.hpp>
#include <atomic>
#include <cstdint>
#include <vk/vulkan.h>

namespace vil {

struct Node {
	std::atomic<std::uintptr_t> next; // Node*
	std::atomic<std::uintptr_t> prev; // Node*
};

struct ImageDescriptor {
	Node imageViewNode;
	Node samplerNode;

	ImageView* imageView;
	Sampler* sampler; // even stored here if immutable in layout
	VkImageLayout layout;
};

struct BufferDescriptor {
	Node node;

	Buffer* buffer;
	VkDeviceSize offset;
	VkDeviceSize range;
};

struct BufferViewDescriptor {
	Node node;
	BufferView* bufferView;
};

inline void erase(Node& node) {
	auto next = node.next.load() & ~1u;
	auto nextPlus = next + 1u;
	while(!node.next.compare_exchange_weak(next, nextPlus)) {
		next = next & ~1u;
		nextPlus = next + 1u;
	}

	// unlink from prev
	// we nknow that node.next can't have been modified in the meantime
	// so this is valid
	auto* prev = reinterpret_cast<Node*>(node.prev.load());
	auto prevNext = prev->next.load() & ~1u;
	while(!prev->next.compare_exchange_weak(prevNext, nextPlus)) {
		prev = reinterpret_cast<Node*>(node.prev.load());
		prevNext = prev->next.load() & ~1u;

		// would also work but might result in more iterations,
		// in case node.prev was really changed. Less atomic loads though.
		// prevNext = prevNext & ~1u;
	}

	// we know that prev wasn't erased since we locked its next pointer
	// we know that *node.next wasn't erased since we locked node.next,
	//   and erasing (*node.next) would have to change the node.next
	//   pointer for unlinking

	// unlink from next
	auto* nextNode = reinterpret_cast<Node*>(next);
	nextNode->prev.store(reinterpret_cast<std::uintptr_t>(prev));
	prev->next.store(next); // release the lock we set with nextPlus
}

inline void insert(Node& root, Node& item) {
	item.prev.store(reinterpret_cast<std::uintptr_t>(&root));

	auto itemAddr = reinterpret_cast<std::uintptr_t>(&item);
	auto nextAddr = root.next.load() & ~1u;

	do {
		item.next.store(nextAddr);
	} while(root.next.compare_exchange_weak(nextAddr, itemAddr + 1));

	reinterpret_cast<Node*>(nextAddr)->prev.store(itemAddr);
	root.next.store(itemAddr);
}

} // namespace vil
