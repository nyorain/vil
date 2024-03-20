#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <variant>

// Utility for VK_EXT_device_fault and VK_EXT_device_address_binding_report
// TODO: this could be factored into its own layer. Might make more sense.
// We want this layer *after* vil then to also catch vil issues.

namespace vil {

struct DeviceAddressMap {
	void init(Device& dev);

	VkDebugUtilsMessengerEXT messenger;
};

} // namespace vil
