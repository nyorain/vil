#include <platform.hpp>
#include <layer.hpp>
#include <data.hpp>
#include <vil_api.h>

// TODO: this dependency here is ugly (we also have it in other places)
// maybe copy the key-string-conversion functions to vil?
#include <swa/swa.h>

namespace vil {

Platform::Platform() {
	auto toggleKeyString = std::getenv("VIL_TOGGLE_KEY");
	if(toggleKeyString) {
		toggleKey_ = (enum VilKey) swa_key_from_name(toggleKeyString);
		if(toggleKey_ == VilKeyNone) {
			dlg_error("Invalid key name: {}", toggleKeyString);
		}
	} else {
		// default value
		toggleKey_ = VilKeyEquals;
	}

	auto focusKeyString = std::getenv("VIL_FOCUS_KEY");
	if(focusKeyString) {
		focusKey_ = (enum VilKey) swa_key_from_name(focusKeyString);
		if(focusKey_ == VilKeyNone) {
			dlg_error("Invalid key name: {}", focusKeyString);
		}
	} else {
		// none by default, rarely needed feature
		focusKey_ = VilKeyNone;
	}
}

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
