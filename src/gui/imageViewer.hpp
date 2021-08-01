#pragma once

#include <util/vec.hpp>
#include <gui/render.hpp>

namespace vil {

class ImageViewer {
public:
	// displayed image
	// VkImageView src {};
	VkExtent3D extent {};
	VkImageType imgType {};
	VkFormat format {};
	VkImageSubresourceRange subresRange {};

	Vec2f offset {}; // in uv coords
	float scale {1.f};

public:
	void reset() { imageDraw_ = {}; }
	void init(Gui& gui);
	void display(Draw& draw);
	const auto& imageDraw() const { return imageDraw_; }

private:
	void drawBackground(VkCommandBuffer cb);
	static DrawGuiImage::Type parseType(VkImageType type, VkFormat format,
		VkImageAspectFlagBits aspect);

private:
	Draw* draw_ {}; // current draw
	Gui* gui_ {};
	Vec2f offset_ {};
	Vec2f size_ {};
	DrawGuiImage imageDraw_ {};
	bool panning_ {};
};

} // namespace vil

