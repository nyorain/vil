#pragma once

#include "fwd.hpp"

#include <vk/vulkan.h>
#include <vk/vk_layer.h>
#include <vk/dispatch_table.h>
#include <string>
#include <vector>

struct swa_display;

namespace vil {

struct Instance {
	VkLayerInstanceDispatchTable dispatch;
	VkInstance handle {};
	PFN_vkSetInstanceLoaderData setInstanceLoaderData {};

	struct {
		bool valid {};
		u32 version {};
		u32 apiVersion {};
		u32 engineVersion {};
		std::string name {};
		std::string engineName {};
	} app;

	// info about enabled extensions
	std::vector<std::string> extensions;
	bool debugUtilsEnabled {};

	// NOTE: this just means that the instance supports them.
	// Usually need to check in device for features, might still not
	// support them!
	bool vulkan11 {};
	bool vulkan12 {};

	~Instance();
};

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(
		const VkInstanceCreateInfo* ci,
		const VkAllocationCallbacks* alloc,
		VkInstance* pInstance);

} // namespace vil
