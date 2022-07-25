#include <platform.hpp>
#include <layer.hpp>
#include <data.hpp>

namespace vil {
// api
VKAPI_ATTR void VKAPI_CALL DestroySurfaceKHR(
		VkInstance                                  instance,
		VkSurfaceKHR                                surface,
		const VkAllocationCallbacks*                pAllocator) {
	if(!surface) {
		return;
	}

	auto platform = moveDataOpt<Platform>(surface); // destroy it
	auto& ini = getData<Instance>(instance);
	ini.dispatch.DestroySurfaceKHR(instance, surface, pAllocator);
}

} // namespace vil
