#pragma once

#include "fwd.hpp"

#include <vkpp/dispatch.hpp>
#include <vkpp/span.hpp>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

struct swa_display;

namespace fuen {

struct Instance {
	vk::DynamicDispatch dispatch;
	VkInstance handle {};

	struct {
		u32 version;
		u32 apiVersion;
		u32 engineVersion;
		std::string name;
		std::string engineName;
	} app;

	// We only create one display per instance.
	// We need to create it before we create the device (and initialize
	// all related data) to query for extensions and queue support.
	swa_display* display {};

	~Instance();
};

} // namespace fuen
