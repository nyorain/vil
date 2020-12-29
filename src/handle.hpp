#pragma once

#include <fwd.hpp>
#include <data.hpp> // for handleToU64
#include <dlg/dlg.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <unordered_set>

#include <vulkan/vulkan.h>

namespace fuen {

struct Handle {
	std::string name;
	// NOTE: should this rather be an unordered multimap?
	// not clear from spec whether setting a tag is intended
	// to replace an old one.
	std::unordered_map<u64, std::vector<std::byte>> tags;
	VkObjectType objectType {};

	Handle() = default;
	Handle(Handle&&) = delete;
	Handle& operator=(Handle&&) = delete;
};

struct DeviceHandle : Handle {
	Device* dev {};

	// A list of all command buffers recordings referencing this handle in their
	// current record state.
	// On destruction, the handle will inform all of them that they
	// are now in an invalid state.
	// NOTE: the dynamic memory allocations we do here could become a
	// serious performance problem. In that case replace it by per-cb
	// 2D-linked-list (linked grid), see node 1648
	std::unordered_set<CommandRecord*> refRecords;

	// Expects that neither the device mutex nor its own mutex is locked.
	~DeviceHandle();

	// Will inform all command buffers that use this handle that they
	// have been invalidated.
	void invalidateCbs();
	void invalidateCbsLocked();
};

const char* name(VkObjectType objectType);
std::string name(const Handle& handle);

struct ObjectTypeHandler {
	static const span<const ObjectTypeHandler*> handlers;

	virtual ~ObjectTypeHandler() = default;
	virtual VkObjectType objectType() const = 0;

	// The following functions may use the device maps directly and
	// can expect the device mutex to be locked.
	virtual std::vector<Handle*> resources(Device& dev, std::string_view search) const = 0;
	virtual Handle* find(Device& dev, u64) const = 0;
};

// And so He spoke:
//  "Letteth us go into the Holy Lands of SFINAE /
//   Where Your Souls may findeth rest /
//   Since'o He - and only Him - in all his Glory /
//   Has forseen this as Our Home!"
template<typename T>
auto handle(const T& handle) -> decltype(handle.handle()) {
	return handle.handle();
}

template<typename T>
auto handle(const T& handle) -> decltype(handle.handle) {
	return handle.handle;
}

// api
VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectNameInfoEXT*        pNameInfo);

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectTagInfoEXT*         pTagInfo);

} // namespace fuen
