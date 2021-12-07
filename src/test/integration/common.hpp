#pragma once

#include "../bugged.hpp"
#include <vk/vulkan.h>
#include <dlg/dlg.hpp>

using u32 = std::uint32_t;

struct VulkanSetup {
	VkInstance ini;
	VkPhysicalDevice phdev;
	VkDevice dev;
	VkQueue queue;
	u32 qfam;
};

extern VulkanSetup gSetup;
inline const VulkanSetup& getSetup() {
	return gSetup;
}

// util
// We can't use vil/util since we don't link directly against vil in
// the integration tests.
inline u32 findLSB(u32 v) {
	// https://stackoverflow.com/questions/757059
	static const int blackMagic[32] = {
		0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
		31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
	};
	return blackMagic[((u32)((v & (~v + 1)) * 0x077CB531U)) >> 27];
}

#define VK_CHECK(x) do {\
		auto result = (x);\
		dlg_assertm(result == VK_SUCCESS, "result: {}", result); \
	} while(0)

