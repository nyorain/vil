#pragma once

#include "fwd.hpp"
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

namespace fuen {

struct Handle {
	std::string name;
	// NOTE: should this rather be an unordered multimap?
	// not clear from spec whether setting a tag is intended
	// to replace an old one.
	std::unordered_map<u64, std::vector<std::byte>> tags;

	Handle() = default;
	Handle(Handle&&) = delete;
	Handle& operator=(Handle&&) = delete;
};

struct DeviceHandle : Handle {
	Device* dev {};
	~DeviceHandle();
};

// api
VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectNameInfoEXT*        pNameInfo);

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(
	VkDevice                                    device,
	const VkDebugUtilsObjectTagInfoEXT*         pTagInfo);

} // namespace fuen
