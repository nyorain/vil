#include "vk/vulkan_core.h"
#include <threadContext.hpp>
#include <util/util.hpp>
#include <util/f16.hpp>
#include <nytl/bytes.hpp>
#include <nytl/vecOps.hpp>
#include <util/dlg.hpp>
#include <vk/typemap_helper.h>
#include <vk/vk_layer.h>
#include <vk/format_utils.h>
#include <vkutil/enumString.hpp>
#include <cmath>
#include <cstdio>

namespace vil {

// high-level stuff
u32 findLSB(u32 v) {
	// https://stackoverflow.com/questions/757059
	static const int blackMagic[32] = {
		0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
		31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
	};
	return blackMagic[((u32)((v & (~v + 1)) * 0x077CB531U)) >> 27];
}

u32 nextPOT(u32 v) {
    dlg_assert(v > 0);

	--v;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	++v;

    return v;
}

bool checkEnvBinary(const char* env, bool defaultValue) {
	auto e = std::getenv(env);
	if(!e) {
		return defaultValue;
	}

	if(std::strcmp(e, "0") == 0u) {
		return false;
	}

	if(std::strcmp(e, "1") == 0u) {
		return true;
	}

	dlg_warn("Environment variable '{}' set to invalid value '{}'. Expected 0 or 1",
		env, e);
	return defaultValue;
}

VkImageType minImageType(VkExtent3D size, unsigned minDim) {
	if(size.depth > 1 || minDim > 2) {
		return VK_IMAGE_TYPE_3D;
	} else if(size.height > 1 || minDim > 1) {
		return VK_IMAGE_TYPE_2D;
	} else {
		return VK_IMAGE_TYPE_1D;
	}
}

VkImageViewType minImageViewType(VkExtent3D size, unsigned layers,
		bool cubemap, unsigned minDim) {
	if(size.depth > 1 || minDim > 2) {
		dlg_assertm(layers <= 1 && cubemap == 0,
			"Layered or cube 3D images are not allowed");
		return VK_IMAGE_VIEW_TYPE_3D;
	}

	if(cubemap) {
		dlg_assert(layers % 6 == 0u);
		return (layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE);
	}

	if(size.height > 1 || minDim > 1) {
		return layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
	} else {
		return layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
	}
}

std::size_t structSize(VkStructureType type) {
	// special layer structures
	switch(type) {
		case VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO: return sizeof(VkLayerInstanceCreateInfo);
		case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO: return sizeof(VkLayerDeviceCreateInfo);
		default: break;
	}

	LvlGenericHeader header {};
	header.sType = type;
	std::size_t size = sizeof(header);

	auto success = castCall(header, [&](auto& casted) { size = sizeof(casted); });
	dlg_assertm(success, "unknown stype {}", type);

	return size;
}

std::unique_ptr<std::byte[]> copyChain(const void* pNext) {
	if(!pNext) {
		return {};
	}

	// first march-through: find needed size
	std::size_t size = 0u;
	auto it = pNext;
	while(it) {
		auto src = static_cast<const VkBaseInStructure*>(it);

		auto ssize = structSize(src->sType);
		dlg_assertm(ssize > 0, "Unknown structure type: {}", src->sType);
		size += ssize;

		it = src->pNext;
	}

	auto buf = std::make_unique<std::byte[]>(size);
	auto offset = std::size_t(0u);

	// second-march-through: copy structure
	VkBaseInStructure* last = nullptr;
	it = pNext;
	while(it) {
		auto src = static_cast<const VkBaseInStructure*>(it);
		auto size = structSize(src->sType);
		dlg_assertm(size > 0, "Unknown structure type: {}", src->sType);

		auto dst = reinterpret_cast<VkBaseInStructure*>(buf.get() + offset);
		// TODO: technicallly UB to not construct object via placement new.
		// In practice, this works everywhere since its only C PODs
		std::memcpy(dst, src, size);
		offset += size;

		if(last) {
			last->pNext = dst;
		} else {
			pNext = dst;
		}

		last = dst;
		it = src->pNext;
	}

	dlg_assert(offset == size);
	return buf;
}

void* copyChainPatch(const void*& pNext, std::unique_ptr<std::byte[]>& buf) {
	if(!pNext) {
		return nullptr;
	}

	buf = copyChainPatch(pNext);
	return static_cast<void*>(buf.get());
}

void* copyChainLocal(ThreadMemScope& memScope, const void* pNext) {
	VkBaseInStructure* last = nullptr;
	void* ret = nullptr;
	auto it = static_cast<const VkBaseInStructure*>(pNext);

	while(it) {
		auto size = structSize(it->sType);
		dlg_assertm_or(size > 0, it = it->pNext; continue,
			"Unknown structure type: {}", it->sType);

		auto dstBuf = memScope.alloc<std::byte>(size);
		auto* dst = reinterpret_cast<VkBaseInStructure*>(dstBuf.data());

		// TODO: technicallly UB to not construct object via placement new.
		// In practice, this works everywhere since its only C PODs
		std::memcpy(dst, it, size);
		dst->pNext = nullptr;

		if(!last) {
			dlg_assert(!ret);
			ret = static_cast<void*>(dst);
		} else {
			last->pNext = dst;
		}

		last = dst;
		it = it->pNext;
	}

	return ret;
}

void writeFile(const char* path, span<const std::byte> buffer, bool binary) {
	dlg_assert(path);
	errno = 0;

	auto* f = std::fopen(path, binary ? "wb" : "w");
	if(!f) {
		dlg_error("Could not open '{}' for writing: {}", path, std::strerror(errno));
		return;
	}

	auto ret = std::fwrite(buffer.data(), 1, buffer.size(), f);
	if(ret != buffer.size()) {
		dlg_error("fwrite on '{}' failed: {}", path, std::strerror(errno));
	}

	std::fclose(f);
}

u32 indexSize(VkIndexType type) {
	// NOTE: When extending here, also extend readIndex
	switch(type) {
		case VK_INDEX_TYPE_UINT16: return 2;
		case VK_INDEX_TYPE_UINT32: return 4;
		case VK_INDEX_TYPE_UINT8_EXT: return 1;
		case VK_INDEX_TYPE_MAX_ENUM:
		case VK_INDEX_TYPE_NONE_KHR:
			return 0;
	}

	return 0;
}

u32 readIndex(VkIndexType type, ReadBuf& data) {
	switch(type) {
		case VK_INDEX_TYPE_UINT16: return read<u16>(data);
		case VK_INDEX_TYPE_UINT32: return read<u32>(data);
		case VK_INDEX_TYPE_UINT8_EXT: return read<u8>(data);
		case VK_INDEX_TYPE_MAX_ENUM:
		case VK_INDEX_TYPE_NONE_KHR:
			dlg_error("invalid index type");
			return 0;
	}

	dlg_error("invalid index type");
	return 0;
}


BufferInterval minMaxInterval(span<const VkBufferImageCopy2KHR> copies, u32 texelSize) {
	VkDeviceSize offset = VkDeviceSize(-1);
	VkDeviceSize end = 0u;

	for(auto& copy : copies) {
		auto stride = copy.bufferRowLength ?
			copy.bufferRowLength : copy.imageExtent.width * texelSize;
		auto hstride = copy.bufferImageHeight ?
			copy.bufferImageHeight : copy.imageExtent.height * stride;
		auto size = VkDeviceSize(hstride * copy.imageExtent.depth);
		offset = std::min(copy.bufferOffset, offset);
		end = std::max(copy.bufferOffset + size, end);
	}

	return {offset, end - offset};
}

BufferInterval minMaxInterval(span<const VkBufferCopy2KHR> copies, bool src) {
	VkDeviceSize offset = VkDeviceSize(-1);
	VkDeviceSize end = 0u;

	for(auto& copy : copies) {
		auto off = src ? copy.srcOffset : copy.dstOffset;
		offset = std::min(off, offset);
		end = std::max(off + copy.size, end);
	}

	return {offset, end - offset};
}

VkImageAspectFlags aspects(VkFormat format) {
	VkImageAspectFlags ret {};
	if(FormatHasDepth(format)) {
		ret |= VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	if(FormatHasStencil(format)) {
		ret |= VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	if(FormatIsColor(format)) {
		ret |= VK_IMAGE_ASPECT_COLOR_BIT;
	}

	return ret;
}

VkImageViewType imageViewForImageType(VkImageType type) {
	switch(type) {
		case VK_IMAGE_TYPE_1D:
			return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
		case VK_IMAGE_TYPE_2D:
			return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		case VK_IMAGE_TYPE_3D:
			return VK_IMAGE_VIEW_TYPE_3D;
		default:
			dlg_error("Unsupported image type");
			return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
	}
}

u32 combineQueueFamilies(span<const u32> queueFams) {
	u32 ret = 0u;
	for(auto qf : queueFams) {
		dlg_assert(qf < 32);
		ret |= (1u << qf);
	}

	return ret;
}

ShaderImageType::Value ShaderImageType::parseType(VkImageType imgType,
		VkFormat format, VkImageAspectFlagBits aspect,
		VkSampleCountFlagBits samples) {
	// NOTE: relies on ordering of DrawGuiImage::Type enum
	using NumType = FORMAT_NUMERICAL_TYPE;
	auto imageTypeFUI = [](auto numt) {
		if(numt == NumType::SINT) return Value::i1;
		else if(numt == NumType::UINT) return Value::u1;
		else return Value::f1;
	};

	Value baseType;

	if(aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
		if(FormatIsSampledFloat(format)) baseType = Value::f1;
		else if(FormatIsUINT(format)) baseType = Value::u1;
		else if(FormatIsSINT(format)) baseType = Value::i1;
		else {
			dlg_error("unreachable");
			return ShaderImageType::count;
		}
	} else {
		auto numt = NumType::NONE;
		if(aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
			numt = FormatDepthNumericalType(format);
		} else if(aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
			numt = FormatStencilNumericalType(format);
		} else {
			dlg_error("unreachable");
			return ShaderImageType::count;
		}

		baseType = imageTypeFUI(numt);
	}

	auto off = 0u;
	switch(imgType) {
		case VK_IMAGE_TYPE_1D:
			dlg_assert(samples == VK_SAMPLE_COUNT_1_BIT);
			off = 0u;
			break;
		case VK_IMAGE_TYPE_2D:
			off = (samples == VK_SAMPLE_COUNT_1_BIT) ? 1u : 2u;
			break;
		case VK_IMAGE_TYPE_3D:
			dlg_assert(samples == VK_SAMPLE_COUNT_1_BIT);
			off = 3u;
			break;
		default:
			dlg_error("unreachable");
			return ShaderImageType::count;
	}

	return Value(unsigned(baseType) + off);
}

} // namespace vil
