// This code is compiled into vil itself. But run in an integration
// test context, i.e. inside our application that correctly
// initializes everything.

#include "internal.hpp"
#include <wrap.hpp>
#include <queue.hpp>
#include <layer.hpp>
#include <util/export.hpp>
#include <vk/dispatch_table_helper.h>

using namespace vil::test;

vil::test::InternalSetup vil::test::gSetup;

extern "C" VIL_EXPORT int vil_runInternalIntegrationTests(
		VkInstance outsideInstance,
		PFN_vkGetInstanceProcAddr outsideGetInstanceProcAddr) {
	// at this point, there should be an instance and device
	{
		std::lock_guard lock(vil::dataMutex);
		dlg_assert(!vil::devByLoaderTable.empty());
		gSetup.vilDev = vil::devByLoaderTable.begin()->second;
	}

	auto& queue = *gSetup.vilDev->queues.front();

	// init setup
	gSetup.dev = undispatch<VkDevice>(*gSetup.vilDev);
	// TODO: shouldn't be used.
	// gSetup.ini = undispatch<VkInstance>(*gSetup.vilDev->ini);
	gSetup.phdev = gSetup.vilDev->phdev; // not wrapped
	gSetup.queue = undispatch<VkQueue>(queue);
	gSetup.qfam = queue.family;
	gSetup.outsideInstance = outsideInstance;

	layer_init_device_dispatch_table(gSetup.dev,
		&gSetup.dispatch, &vil::GetDeviceProcAddr);
	layer_init_instance_dispatch_table(gSetup.outsideInstance,
		&gSetup.iniDispatch, outsideGetInstanceProcAddr);

	// Runn all integration tests
	return vil::bugged::Testing::get().run("int_");
}

