#include <fault.hpp>
#include <device.hpp>
#include <layer.hpp>
#include <util/util.hpp>

namespace vil {

void DeviceAddressMap::init(Device& dev) {
	dlg_trace("initialized DeviceAddressMap");
	dlg_assert(contains(dev.ini->extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
	dlg_assert(dev.ini->dispatch.CreateDebugUtilsMessengerEXT);

	// TODO
}

} // namespace vil
