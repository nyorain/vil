// header-only utility
// used for external as well as internal integration tests
// must not use symbols defined in vil

#include <vk/vulkan.h>
#include <vk/dispatch_table.h>
#include <vk/object_types.h>
#include <util/dlg.hpp>
#include <util/handleCast.hpp>
#include <nytl/span.hpp>

using nytl::span;

#define VK_CHECK(x) do {\
		auto result = (x);\
		dlg_assertm(result == VK_SUCCESS, "result: {}", result); \
	} while(0)

namespace tut {

// all inline util functions
using u32 = std::uint32_t;

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

struct Setup {
	VkInstance ini;
	VkPhysicalDevice phdev;
	VkDevice dev;
	VkQueue queue;
	VkQueue queue2; // alternative, separate queue
	u32 qfam;
	u32 qfam2; // for queue2

	VkLayerInstanceDispatchTable iniDispatch;
	VkLayerDispatchTable dispatch;
};

struct TextureCreation {
	VkImageCreateInfo ici {};
	VkImageViewCreateInfo ivi {};

	inline TextureCreation(VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
};

struct Texture {
	Setup* setup {};
	VkImage image {};
	VkImageView imageView {};
	VkDeviceMemory devMem {};

	inline Texture(Setup&, TextureCreation);

	~Texture() {
		if(setup) {
			setup->dispatch.DestroyImageView(setup->dev, imageView, nullptr);
			setup->dispatch.DestroyImage(setup->dev, image, nullptr);
			setup->dispatch.FreeMemory(setup->dev, devMem, nullptr);
		}
	}

	Texture(Texture&& rhs) noexcept { swap(*this, rhs); }
	Texture& operator=(Texture rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}

	friend void swap(Texture& a, Texture& b) noexcept {
		using std::swap;
		swap(a.setup, b.setup);
		swap(a.image, b.image);
		swap(a.imageView, b.imageView);
		swap(a.devMem, b.devMem);
	}
};

inline TextureCreation::TextureCreation(VkFormat format) {
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.arrayLayers = 1u;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.extent = {1024u, 1024u, 1u};
	ici.format = format;
	ici.mipLevels = 10u;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_STORAGE_BIT;

	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.format = format;
	ivi.components.r = VK_COMPONENT_SWIZZLE_R;
	ivi.components.g = VK_COMPONENT_SWIZZLE_G;
	ivi.components.b = VK_COMPONENT_SWIZZLE_B;
	ivi.components.a = VK_COMPONENT_SWIZZLE_A;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.baseMipLevel = 0;
	ivi.subresourceRange.levelCount = 1;
	ivi.subresourceRange.baseArrayLayer = 0;
	ivi.subresourceRange.layerCount = 1;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.flags = 0;
	ivi.image = {};
}

inline Texture::Texture(Setup& stp, TextureCreation tc) {
	setup = &stp;
	VK_CHECK(stp.dispatch.CreateImage(stp.dev, &tc.ici, nullptr, &image));

	VkMemoryRequirements memReqs;
	stp.dispatch.GetImageMemoryRequirements(stp.dev, image, &memReqs);

	VkMemoryAllocateInfo mai {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = memReqs.size;
	mai.memoryTypeIndex = findLSB(memReqs.memoryTypeBits);
	VK_CHECK(stp.dispatch.AllocateMemory(stp.dev, &mai, nullptr, &devMem));
	VK_CHECK(stp.dispatch.BindImageMemory(stp.dev, image, devMem, 0u));

	tc.ivi.image = image;
	VK_CHECK(stp.dispatch.CreateImageView(stp.dev, &tc.ivi, NULL, &imageView));
}

struct Buffer {
	Setup* setup {};
	VkBuffer buffer {};
	VkDeviceMemory devMem {};

	inline Buffer(Setup&, VkDeviceSize size, VkImageUsageFlags usage);
	~Buffer() {
		if(setup) {
			setup->dispatch.DestroyBuffer(setup->dev, buffer, nullptr);
			setup->dispatch.FreeMemory(setup->dev, devMem, nullptr);
		}
	}
};

inline Buffer::Buffer(Setup& stp, VkDeviceSize size, VkImageUsageFlags usage) : setup(&stp) {
	VkBufferCreateInfo bci {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bci.size = size;
	bci.usage = usage;
	VK_CHECK(setup->dispatch.CreateBuffer(setup->dev, &bci, nullptr, &buffer));

	VkMemoryRequirements memReqs;
	setup->dispatch.GetBufferMemoryRequirements(setup->dev, buffer, &memReqs);

	VkMemoryAllocateInfo mai {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = memReqs.size;
	mai.memoryTypeIndex = findLSB(memReqs.memoryTypeBits);
	VK_CHECK(setup->dispatch.AllocateMemory(setup->dev, &mai, nullptr, &devMem));
	VK_CHECK(setup->dispatch.BindBufferMemory(setup->dev, buffer, devMem, 0u));
}

struct RenderPassInfo {
	VkRenderPassCreateInfo renderPass;
	std::vector<VkAttachmentDescription> attachments;
	std::vector<VkSubpassDescription> subpasses;
	std::vector<VkSubpassDependency> dependencies;

	std::vector<std::vector<VkAttachmentReference>> attachmentRefs;

	const VkRenderPassCreateInfo& info() {
		renderPass.pAttachments = attachments.data();
		renderPass.attachmentCount = attachments.size();
		renderPass.pSubpasses = subpasses.data();
		renderPass.subpassCount = subpasses.size();
		renderPass.dependencyCount = dependencies.size();
		renderPass.pDependencies = dependencies.data();
		return renderPass;
	}
};

constexpr inline bool HasDepth(VkFormat fmt) {
	switch(fmt) {
		case VK_FORMAT_D16_UNORM_S8_UINT:
		case VK_FORMAT_D16_UNORM:
		case VK_FORMAT_D24_UNORM_S8_UINT:
		case VK_FORMAT_X8_D24_UNORM_PACK32:
		case VK_FORMAT_D32_SFLOAT_S8_UINT:
		case VK_FORMAT_D32_SFLOAT:
			return true;
		default:
			return false;
	}
}

// Creates a simple RenderPassCreate info (+ everything what is needed)
// from a simple meta-description.
// - no dependencies or flags or something
// - initialLayout always 'undefined'
// - finalLayout always 'shaderReadOnlyOptimal'
// - loadOp clear, storeOp store
// Passes contains the ids of the attachments used by the passes.
// Depending on the format they will be attached as color or depth
// attachment. Input, preserve attachments or multisampling not
// supported here, they can be added manually to the returned description
// afterwards though.
inline RenderPassInfo renderPassInfo(span<const VkFormat> formats,
		span<const span<const unsigned>> passes) {
	RenderPassInfo rpi {};
	rpi.renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;

	for(auto f : formats) {
		auto& a = rpi.attachments.emplace_back();
		a.format = f;
		a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		a.samples = VK_SAMPLE_COUNT_1_BIT;
	}

	for(auto pass : passes) {
		auto& subpass = rpi.subpasses.emplace_back();
		auto& colorRefs = rpi.attachmentRefs.emplace_back();
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;

		bool depth = false;
		for(auto id : pass) {
			dlg_assert(id < rpi.attachments.size());
			auto format = formats[id];
			if(HasDepth(format)) {
				dlg_assertm(!depth, "More than one depth attachment");
				depth = true;
				auto& ref = rpi.attachmentRefs.emplace_back().emplace_back();
				ref.attachment = id;
				ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
				subpass.pDepthStencilAttachment = &ref;
			} else {
				auto& ref = colorRefs.emplace_back();
				ref.attachment = id;
				ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			}
		}

		subpass.pColorAttachments = colorRefs.data();
		subpass.colorAttachmentCount = colorRefs.size();
	}

	return rpi;
}

inline VkSamplerCreateInfo linearSamplerCI() {
	VkSamplerCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sci.magFilter = VK_FILTER_LINEAR;
	sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.minLod = -1000;
	sci.maxLod = 1000;
	sci.maxAnisotropy = 1.0f;
	return sci;
}


template<typename H>
void setDebugName(const Setup& stp, H handle, const char* name) {
	VkDebugUtilsObjectNameInfoEXT ni {};
	ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	ni.objectType = VkHandleInfo<H>::kVkObjectType;
	ni.objectHandle = vil::handleToU64(handle);
	ni.pObjectName = name;
	VK_CHECK(stp.dispatch.SetDebugUtilsObjectNameEXT(stp.dev, &ni));
}

} // namespace tut
