#pragma once

#include "fwd.hpp"

#include <vkpp/dispatch.hpp>
#include <vkpp/span.hpp>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

namespace fuen {

struct Instance {
	vk::DynamicDispatch dispatch;
	VkInstance handle;

	struct {
		u32 version;
		u32 apiVersion;
		u32 engineVersion;
		std::string name;
		std::string engineName;
	} app;
};

} // namespace fuen
