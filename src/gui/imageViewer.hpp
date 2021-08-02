#pragma once

#include <util/vec.hpp>
#include <gui/render.hpp>

namespace vil {

class ImageViewer {
public:
	// displayed image
	VkExtent3D extent {};
	VkImageType imgType {};
	VkFormat format {};
	VkImageSubresourceRange subresRange {};

	Vec2f offset {}; // in uv coords
	float scale {1.f};

	VkImage src {};
	VkImageLayout initialImageLayout {};
	VkImageLayout finalImageLayout {};
	bool copyTexel {true};

public:
	void reset() { imageDraw_ = {}; }
	void init(Gui& gui);
	void display(Draw& draw);
	const auto& imageDraw() const { return imageDraw_; }

	u32 level() const { return imageDraw_.level; }
	u32 layer() const { return imageDraw_.layer; } // for 3D: slice
	VkImageAspectFlagBits aspect() const { return imageDraw_.aspect; }

private:
	// Called during recording before the image is rendered via imgui.
	// Will perform transitions, if needed, and draw the display area background.
	void recordPreImage(VkCommandBuffer cb);
	void drawBackground(VkCommandBuffer cb);

	// Called during recording after the image was rendered via imgui.
	// Will perform transitions, if needed.
	void recordPostImage(Draw& draw);

	void doCopy(VkCommandBuffer cb, Draw& draw);
	void copyComplete(Draw&);

	static DrawGuiImage::Type parseType(VkImageType type, VkFormat format,
		VkImageAspectFlagBits aspect);

private:
	Draw* draw_ {}; // current draw
	Gui* gui_ {};
	Vec2f offset_ {};
	Vec2f size_ {};
	DrawGuiImage imageDraw_ {};
	bool panning_ {};

	struct Readback {
		OwnBuffer own;
		Draw* pending {};

		bool valid {};
		VkOffset2D texel {};
		unsigned layer {};
		unsigned level {};
	};

	std::vector<Readback> readbacks_;
	std::optional<unsigned> lastReadback_ {};
	VkOffset2D readTexelOffset_ {};
};

} // namespace vil

