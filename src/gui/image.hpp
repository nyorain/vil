#pragma once

#include <device.hpp>

namespace fuen {

struct ImageGui {
	void draw();

	Image* image_;
	VkImageSubresourceRange subres_;
	VkImageView view_;
};

} // namespace fuen

