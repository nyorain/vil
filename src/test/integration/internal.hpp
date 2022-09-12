// Common header for internal integration testing
// Here, we can use vil functionality.

#include "../bugged.hpp"
#include "util.hpp"
#include <fwd.hpp>
#include <wrap.hpp>
#include <data.hpp>
#include <shared_mutex>

namespace vil::test {

struct InternalSetup : tut::Setup {
	Device* vilDev;
	Queue* vilQueue;
	Queue* vilQueue2;

	// We need the original instance (as it was created) to call
	// instance functions here
	VkInstance outsideInstance;
};

extern InternalSetup gSetup;

// Given non-wrapped, dispatchable handle dst (e.g. vil::Instance,
// vil::Device, vil::Queue), returns the external Vk handle that is a
// associated with it, i.e. the VkInstance/VkDevice/VkQueue that vil
// expects to be called with.
template<typename T, typename O>
T undispatch(O& dst) {
	std::shared_lock lock(dataMutex);
	for(auto& entry : dispatchableTable) {
		if(entry.second == &dst) {
			return u64ToHandle<T>(entry.first);
		}
	}

	throw std::runtime_error("Invalid handle");
}

} // namespace vil::test
