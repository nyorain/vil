#include <imageLayout.hpp>
#include <random>
#include <algorithm>
#include "../bugged.hpp"
#include "../approx.hpp"

vil::ImageSubresourceLayout change(unsigned levelStart, unsigned levelCount,
		unsigned layerStart, unsigned layerCount, VkImageLayout layout,
		VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT) {
	vil::ImageSubresourceLayout ret {};
	ret.layout = layout;
	ret.range.aspectMask = aspects;
	ret.range.baseArrayLayer = layerStart;
	ret.range.layerCount = layerCount;
	ret.range.baseMipLevel = levelStart;
	ret.range.levelCount = levelCount;
	return ret;
}

TEST(unit_imageLayout) {
	VkImageCreateInfo ici {};
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.arrayLayers = 32u;
	ici.mipLevels = 10u;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	auto state = std::vector{vil::initialLayout(ici)};

	for(auto level = 0u; level < ici.mipLevels; ++level) {
		for(auto layer = 0u; layer < ici.arrayLayers; ++layer) {
			EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, level, layer}),
				VK_IMAGE_LAYOUT_UNDEFINED);
		}
	}

	// some changes
	auto changes = {
		change(0, 1, 0, 32, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
		change(1, 1, 1, 1, VK_IMAGE_LAYOUT_GENERAL),
	};

	vil::apply(state, changes);
	vil::checkForErrors(state, ici);

	EXPECT(state.size(), 5u);

	EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1}), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 31}), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0}), VK_IMAGE_LAYOUT_UNDEFINED);
	EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1}), VK_IMAGE_LAYOUT_GENERAL);
	EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, 2, 0}), VK_IMAGE_LAYOUT_UNDEFINED);
	EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, 9, 0}), VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST(unit_imageLayout_simplify) {
	VkImageCreateInfo ici {};
	ici.format = VK_FORMAT_R8G8B8A8_UNORM;
	ici.arrayLayers = 320u;
	ici.mipLevels = 10u;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	auto state = std::vector{vil::initialLayout(ici)};

	std::random_device rd;
	std::mt19937 e2(rd());
	std::uniform_int_distribution<> distLevel(0, ici.mipLevels - 1);
	std::uniform_int_distribution<> distLayer(0, ici.arrayLayers - 1);

	for(auto i = 0u; i < 1000; ++i) {
		auto layerStart = distLayer(e2);
		auto levelStart = distLevel(e2);
		auto layerCount = std::min<unsigned>(1 + distLayer(e2) / 10, ici.arrayLayers - layerStart);
		auto levelCount = std::min<unsigned>(1 + distLevel(e2) / 2, ici.mipLevels - levelStart);

		auto c = change(levelStart, levelCount, layerStart, layerCount, VK_IMAGE_LAYOUT_UNDEFINED);
		vil::apply(state, {{c}});
	}

	vil::checkForErrors(state, ici);

	dlg_info("state size before simplify: {}", state.size());
	vil::simplify(state);

	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());
	vil::simplify(state);
	dlg_info("state size after simplify: {}", state.size());

	vil::checkForErrors(state, ici);

	// for(auto i = 0u; i < 100; ++i) {
	// 	unsigned layer = distLayer(e2);
	// 	unsigned level = distLevel(e2);
	// 	EXPECT(vil::layout(state, {VK_IMAGE_ASPECT_COLOR_BIT, level, layer}), VK_IMAGE_LAYOUT_GENERAL);
	// }
}

