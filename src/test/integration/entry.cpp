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
	// NOTE: we don't strictly need the mutex here as long as integration
	// test is single-threaded but keep it for good practice.
	{
		std::lock_guard lock(vil::dataMutex);
		dlg_assert(!vil::devByLoaderTable.empty());
		gSetup.vilDev = vil::devByLoaderTable.begin()->second;
	}

	gSetup.vilDev->testing = true;

	dlg_assert(gSetup.vilDev->queues.size() == 2u);
	gSetup.vilQueue = gSetup.vilDev->queues[0].get();
	gSetup.vilQueue2 = gSetup.vilDev->queues[1].get();

	// init setup
	gSetup.dev = undispatch<VkDevice>(*gSetup.vilDev);
	// TODO: shouldn't be used.
	// gSetup.ini = undispatch<VkInstance>(*gSetup.vilDev->ini);
	gSetup.phdev = gSetup.vilDev->phdev; // not wrapped
	gSetup.queue = undispatch<VkQueue>(*gSetup.vilQueue);
	gSetup.queue2 = undispatch<VkQueue>(*gSetup.vilQueue2);
	gSetup.qfam = gSetup.vilQueue->family;
	gSetup.qfam2 = gSetup.vilQueue2->family;
	gSetup.outsideInstance = outsideInstance;

	layer_init_device_dispatch_table(gSetup.dev,
		&gSetup.dispatch, &vil::GetDeviceProcAddr);
	layer_init_instance_dispatch_table(gSetup.outsideInstance,
		&gSetup.iniDispatch, outsideGetInstanceProcAddr);

	// Runn all integration tests
	return vil::bugged::Testing::get().run("int_");
}

