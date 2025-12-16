#include <util/chain.hpp>
#include <threadContext.hpp>
#include <vk/typemap_helper.h>
#include <vk/vk_layer.h>
#include <util/dlg.hpp>

namespace vil {

template<typename F> bool extCastCall(const LvlGenericHeader& header, F&& f) {
	switch(header.sType) {
		case VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO: f(reinterpret_cast<const VkLayerInstanceCreateInfo&>(header)); return true;
		case VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO: f(reinterpret_cast<const VkLayerDeviceCreateInfo&>(header)); return true;
		default: break;
	}

	return castCall(header, std::forward<F>(f));
}

std::size_t structSize(VkStructureType type) {
	LvlGenericHeader header {};
	header.sType = type;
	std::size_t size = sizeof(header);

	auto success = extCastCall(header, [&](auto& casted) { size = sizeof(casted); });
	dlg_assertm(success, "unknown stype {}", type);

	return size;
}

// AllocFunc: std::byte*(std::size_t size)
template<typename T, typename AllocFunc>
T& alloc(AllocFunc& f) {
	auto* ptr = f(sizeof(T));
	return *std::launder(reinterpret_cast<T*>(ptr));
}

template<typename T, typename AllocFunc>
T& copy(const T& obj, AllocFunc& f) {
	auto* ptr = f(sizeof(T));
	auto& dst = *std::launder(reinterpret_cast<T*>(ptr));
	dst = obj;
	return dst;
}

template<typename T, typename AllocFunc>
void copyInPlace(T*& src, AllocFunc& f) {
	if(!src) {
		return;
	}

	using RT = std::remove_cv_t<T>;
	auto* ptr = f(sizeof(RT));
	auto& dst = *std::launder(reinterpret_cast<RT*>(ptr));
	dst = *src; // copy *src object
	src = &dst; // set src pointer to newly allocated dst
}

template<typename T, typename AllocFunc>
void copyArrayInPlace(T*& src, u32 count, AllocFunc& f) {
	if(count == 0u) {
		return;
	}

	dlg_assert(src);

	using RT = std::remove_cv_t<T>;
	auto* ptr = f(sizeof(RT) * count);
	auto* dst = std::launder(reinterpret_cast<RT*>(ptr));
	std::copy(src, src + count, dst);
	src = dst; // set src pointer to newly allocated dst
}

template<typename T, typename AllocFunc>
void patchChainElement(T&, AllocFunc& f) {
	(void) f;
	// default impl: nop
}

template<typename AllocFunc>
void patchChainElement(VkSubpassDescriptionDepthStencilResolve& obj, const AllocFunc& f) {
	copyInPlace(obj.pDepthStencilResolveAttachment, f);
}

template<typename AllocFunc>
void patchChainElement(VkPipelineVertexInputDivisorStateCreateInfo& obj, const AllocFunc& f) {
	copyArrayInPlace(obj.pVertexBindingDivisors, obj.vertexBindingDivisorCount, f);
}

template<typename AllocFunc>
LvlGenericModHeader* copyChainElement(const LvlGenericHeader& input, const AllocFunc& f) {
	LvlGenericModHeader* ret {};
	auto success = extCastCall(input, [&](auto& casted) {
		auto& copied = copy(casted, f);
		patchChainElement(copied, f);
		ret = reinterpret_cast<LvlGenericModHeader*>(&copied);
	});

	dlg_assertm(success, "unknown stype {}", input.sType);
	return ret;
}

// copyChain
template<typename T>
std::size_t patchAllocSize(const T& obj) {
	std::size_t ret = 0u;
	ThreadMemScope tms;
	auto countFunc = [&](std::size_t size) {
		ret += size;
		return tms.allocBytes(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	};

	auto cpy = obj;
	patchChainElement(cpy, countFunc);
	return ret;
}

std::size_t structAndPatchSize(VkStructureType type) {
	LvlGenericHeader header {};
	header.sType = type;
	std::size_t size = 0u;

	auto success = extCastCall(header, [&](auto& casted) {
		size = sizeof(casted);
		size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		size += patchAllocSize(casted);
	});
	dlg_assertm(success, "unknown stype {}", type);

	return size;
}

// AllocFunc: std::byte*(std::size_t size)
template<typename AllocFunc>
void* copyChain(const void* pNext, const AllocFunc& alloc) {
	LvlGenericModHeader* last = nullptr;
	void* ret = nullptr;
	auto it = pNext;
	while(it) {
		auto* src = static_cast<const LvlGenericHeader*>(it);
		auto* copied = copyChainElement(*src, alloc);

		// copying can fail for unknown structs. They are just omitted.
		if(copied) {
			if(last) {
				last->pNext = copied;
			} else {
				ret = copied;
			}

			last = copied;
		}

		it = src->pNext;
	}

	return ret;
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

		auto ssize = structAndPatchSize(src->sType);
		size += ssize;
		size = align(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);

		it = src->pNext;
	}

	if(!size) {
		return {};
	}

	// alloc needed bytes
	auto buf = std::make_unique<std::byte[]>(size);
	auto offset = std::size_t(0u);

	// now copy everything into the buffer
	auto allocFunc = [&](std::size_t allocSize) -> std::byte* {
		dlg_assert(offset + allocSize <= size);
		offset = align(offset, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
		auto* ptr = buf.get() + offset;
		offset += allocSize;
		return ptr;
	};

	auto* ret = copyChain(pNext, allocFunc);
	dlg_assert(ret == buf.get());
	dlg_assertm(align(offset, __STDCPP_DEFAULT_NEW_ALIGNMENT__) == size,
		"offset {}, size {}", offset, size);
	return buf;
}

void* copyChainPatch(const void*& pNext, std::unique_ptr<std::byte[]>& buf) {
	if(!pNext) {
		return nullptr;
	}

	buf = copyChainPatch(pNext);
	return static_cast<void*>(buf.get());
}

void* copyChainLocal(LinAllocScope& memScope, const void* pNext) {
	auto allocFunc = [&](std::size_t size) {
		return memScope.allocBytes(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	};

	return copyChain(pNext, allocFunc);
}

void* copyChain(LinAllocator& allocator, const void* pNext) {
	auto allocFunc = [&](std::size_t size) {
		return allocator.allocate(size, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	};

	return copyChain(pNext, allocFunc);
}


} // namespace
