#include "./internal.hpp"
#include <gui/gui.hpp>
#include <swapchain.hpp>
#include <layer.hpp>
#include <util/export.hpp>
#include <vil_api.h>

// From vil/api.cpp
extern "C" VIL_EXPORT VilOverlay vilCreateOverlayForLastCreatedSwapchain(VkDevice vkDevice);
extern "C" VIL_EXPORT void vilOverlayShow(VilOverlay overlay, bool show);

namespace vil::test {

TEST(int_gui) {
	auto& stp = gSetup;

	// note: we try to enable it during instance setup in integration/main.cpp
	auto hasHeadlessSurface = contains(stp.vilDev->ini->extensions,
		VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
	if(!hasHeadlessSurface) {
		dlg_trace("headless surface ext not supported; skipping test");
		return;
	}

	dlg_assert(!stp.vilDev->swapchain());

	// create surface
	VkHeadlessSurfaceCreateInfoEXT hsci {};
	hsci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;

	VkSurfaceKHR surface {};
	VK_CHECK(stp.iniDispatch.CreateHeadlessSurfaceEXT(stp.outsideInstance,
		&hsci, nullptr, &surface));

	// NOTE: we know that these are supported by the mock driver.
	//   add correct prop querying here if we want to test with other drivers
	//   in the future.
	VkSwapchainCreateInfoKHR sci {};
	sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.imageExtent = {800, 500};
	sci.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
	sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	sci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	sci.minImageCount = 1u;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.imageArrayLayers = 1u;
	sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sci.surface = surface;
	VkSwapchainKHR swapchain;
	VK_CHECK(stp.dispatch.CreateSwapchainKHR(stp.dev, &sci, nullptr, &swapchain));

	dlg_assert(stp.vilDev->swapchain());
	dlg_assert(!stp.vilDev->gui());

	auto& vilSwapchain = *stp.vilDev->swapchain();
	dlg_assert(vilSwapchain.frameTimings.empty());
	dlg_assert(!vilSwapchain.lastPresent);
	dlg_assert(vilSwapchain.images.size() == 1u);

	// create gui via vil api
	auto overlay = vilCreateOverlayForLastCreatedSwapchain(stp.dev);
	vilOverlayShow(overlay, true);

	dlg_assert(stp.vilDev->gui());
	auto& gui = *stp.vilDev->gui();
	dlg_assert(gui.visible());

	// TODO: render an (empty) frame just to cover basic rendering path
	// of gui.

	// cleanup
	stp.dispatch.DestroySwapchainKHR(stp.dev, swapchain, nullptr);
	stp.iniDispatch.DestroySurfaceKHR(stp.outsideInstance, surface, nullptr);
}

} // namespace vil::test
