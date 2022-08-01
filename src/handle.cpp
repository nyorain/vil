#include <handle.hpp>
#include <wrap.hpp>
#include <data.hpp>
#include <queue.hpp>
#include <device.hpp>
#include <handles.hpp>
#include <accelStruct.hpp>
#include <vk/enumString.hpp>
#include <util/util.hpp>
#include <string_view>

namespace vil {

const char* name(VkObjectType objectType) {
	switch(objectType) {
		case VK_OBJECT_TYPE_IMAGE: return "Image";
		case VK_OBJECT_TYPE_IMAGE_VIEW: return "ImageView";
		case VK_OBJECT_TYPE_SAMPLER: return "Sampler";
		case VK_OBJECT_TYPE_BUFFER: return "Buffer";
		case VK_OBJECT_TYPE_BUFFER_VIEW: return "BufferView";
		case VK_OBJECT_TYPE_DEVICE_MEMORY: return "DeviceMemory";
		case VK_OBJECT_TYPE_PIPELINE: return "Pipeline";
		case VK_OBJECT_TYPE_PIPELINE_LAYOUT: return "PipelineLayout";
		case VK_OBJECT_TYPE_DESCRIPTOR_SET: return "DescriptorSet";
		case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT: return "DescriptorSetLayout";
		case VK_OBJECT_TYPE_DESCRIPTOR_POOL: return "DescriptorPool";
		case VK_OBJECT_TYPE_COMMAND_POOL: return "CommandPool";
		case VK_OBJECT_TYPE_COMMAND_BUFFER: return "CommandBuffer";
		case VK_OBJECT_TYPE_FENCE: return "Fence";
		case VK_OBJECT_TYPE_SEMAPHORE: return "Semaphore";
		case VK_OBJECT_TYPE_EVENT: return "Event";
		case VK_OBJECT_TYPE_QUERY_POOL: return "QueryPool";
		case VK_OBJECT_TYPE_SHADER_MODULE: return "ShaderModule";
		case VK_OBJECT_TYPE_QUEUE: return "Queue";
		case VK_OBJECT_TYPE_SWAPCHAIN_KHR: return "Swapchain";
		case VK_OBJECT_TYPE_RENDER_PASS: return "RenderPass";
		case VK_OBJECT_TYPE_FRAMEBUFFER: return "Framebuffer";
		case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE: return "DescriptorUpdateTemplate";
		case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return "AccelerationStructure";
		default: return "?";
	}
}

std::string name(const Handle& handle, bool addType, bool perTypeDefault) {
	const auto hn = name(handle.objectType);

	std::string name;
	if(addType) {
		name += dlg::format("{}", hn);
	}

	if(!handle.name.empty()) {
		if(!name.empty()) {
			name += ' ';
		}

		name += handle.name;
	} else if(perTypeDefault) {
		if(!name.empty()) {
			name += ' ';
		}

		if(handle.objectType == VK_OBJECT_TYPE_IMAGE) {
			name += defaultName(static_cast<const Image&>(handle));
		} else if(handle.objectType == VK_OBJECT_TYPE_IMAGE_VIEW) {
			name += defaultName(static_cast<const ImageView&>(handle));
		} else if(name.empty()) {
			// TODO: not sure if good idea
			name = hn;
			// name = dlg::format("{}{}", std::hex, handleToU64(&handle));
		}
	}

	return name;
}

// type handlers
bool matchesSearch(Handle& handle, std::string_view search) {
	if(search.empty()) {
		return true;
	}

	// case-insensitive search
	auto label = name(handle);

	// TODO: seems msvc can't handle our CI trait -.-
	return (label.find(search) != label.npos);

	/*
	using StringViewCI = std::basic_string_view<char, CharTraitsCI>;
	auto nameCI = StringViewCI(label.data(), label.size());
	auto searchCI = StringViewCI(search.data(), search.size());

	// TODO: better matching. Support regex?
	return (nameCI.find(searchCI) != nameCI.npos);
	*/
}

template<typename... Args>
std::vector<Handle*> findHandles(const std::unordered_map<Args...>& map,
		std::string_view search) {
	std::vector<Handle*> ret;
	for(auto& entry : map) {
		auto& handle = *entry.second;
		if(!matchesSearch(handle, search)) {
			continue;
		}

		ret.push_back(&handle);
	}

	std::sort(ret.begin(), ret.end());
	return ret;
}

template<typename... Args>
std::vector<Handle*> findHandles(const std::unordered_set<Args...>& set,
		std::string_view search) {
	std::vector<Handle*> ret;
	for(auto& entry : set) {
		auto& handle = *entry;
		if(!matchesSearch(handle, search)) {
			continue;
		}

		ret.push_back(&handle);
	}

	std::sort(ret.begin(), ret.end());
	return ret;
}

template<VkObjectType OT, typename HT, auto DevMapPtr>
struct ObjectTypeMapImpl : ObjectTypeHandler {
	static const ObjectTypeMapImpl instance;

	VkObjectType objectType() const override { return OT; }
	Handle* find(Device& dev, u64 id, u64& fwdID) const override {
		// TODO: should probably implement this using vil::get so we
		// automatically just unwrap instead of using the map where possible.
		using VKHT = decltype(vil::handle(std::declval<HT>()));
		auto vkht = u64ToHandle<VKHT>(id);

		/*
		auto& map = (dev.*DevMapPtr).inner;
		auto it = map.find(vkht);
		if(it == map.end()) {
			return nullptr;
		}

		if constexpr(OT == VK_OBJECT_TYPE_COMMAND_BUFFER) {
			fwdID = handleToU64(it->second->handle());
		} else {
			fwdID = handleToU64(it->second->handle);
		}

		return &*it->second;
		*/

		auto& handle = getLocked(dev, vkht);

		if constexpr(OT == VK_OBJECT_TYPE_COMMAND_BUFFER) {
			fwdID = handleToU64(handle.handle());
		} else {
			fwdID = handleToU64(handle.handle);
		}

		return &handle;
	}
	std::vector<Handle*> resources(Device& dev, std::string_view search) const override {
		return findHandles((dev.*DevMapPtr).inner, search);
	}
	void visit(ResourceVisitor& visitor, Handle& handle) const override {
		dlg_assert(handle.objectType == objectType());
		return visitor.visit(static_cast<HT&>(handle));
	}
};

struct DescriptorSetTypeImpl : ObjectTypeHandler {
	static const DescriptorSetTypeImpl instance;

	// when we wrap descriptor sets, we don't insert them into the
	// device map

	VkObjectType objectType() const override { return VK_OBJECT_TYPE_DESCRIPTOR_SET; }
	Handle* find(Device& dev, u64 id, u64& fwdID) const override {
		if(HandleDesc<VkDescriptorSet>::wrap) {
			// TODO PERF: extremely slow, iterating over *all* descriptor sets
			// via linked lists
			auto vkh = u64ToHandle<VkDescriptorSet>(id);
			for(auto& [h, pool] : dev.dsPools.inner) {
				for(auto it = pool->usedEntries; it; it = it->next) {
					auto& set = *it->set;
					if(castDispatch<VkDescriptorSet>(set) == vkh) {
						fwdID = handleToU64(set.handle);
						return &set;
					}
				}
			}
		} else {
			auto& map = dev.descriptorSets.inner;
			auto it = map.find(u64ToHandle<VkDescriptorSet>(id));
			if(it == map.end()) {
				return nullptr;
			}

			fwdID = handleToU64(it->second->handle);
			return it->second;
		}

		return nullptr;
	}
	std::vector<Handle*> resources(Device& dev, std::string_view search) const override {
		if(!HandleDesc<VkDescriptorSet>::wrap) {
			return findHandles(dev.descriptorSets.inner, search);
		}

		std::vector<Handle*> ret;
		for(auto& [h, pool] : dev.dsPools.inner) {
			for(auto it = pool->usedEntries; it; it = it->next) {
				if(matchesSearch(*it->set, search)) {
					ret.push_back(it->set);
				}
			}
		}
		return ret;
	}
	void visit(ResourceVisitor& visitor, Handle& handle) const override {
		dlg_assert(handle.objectType == objectType());
		return visitor.visit(static_cast<Queue&>(handle));
	}
};

struct QueueTypeImpl : ObjectTypeHandler {
	static const QueueTypeImpl instance;

	VkObjectType objectType() const override { return VK_OBJECT_TYPE_QUEUE; }
	Handle* find(Device& dev, u64 id, u64& fwdID) const override {
		// NOTE: alternatively (maybe faster but does not really matter)
		// we could use findData<Queue>((VkQueue) id)
		for(auto& queue : dev.queues) {
			if(handleToU64(queue->handle) == id) {
				fwdID = handleToU64(queue->handle);
				return queue.get();
			}
		}

		return nullptr;
	}
	std::vector<Handle*> resources(Device& dev, std::string_view search) const override {
		std::vector<Handle*> ret;
		for(auto& queue : dev.queues) {
			// We never return queues created by us, they don't count as
			// resources.
			if(queue->createdByUs || !matchesSearch(*queue, search)) {
				continue;
			}

			ret.push_back(queue.get());
		}

		return ret;
	}
	void visit(ResourceVisitor& visitor, Handle& handle) const override {
		dlg_assert(handle.objectType == objectType());
		return visitor.visit(static_cast<Queue&>(handle));
	}
};

template<VkObjectType OT, typename HT, auto DevMapPtr>
const ObjectTypeMapImpl<OT, HT, DevMapPtr> ObjectTypeMapImpl<OT, HT, DevMapPtr>::instance;

const QueueTypeImpl QueueTypeImpl::instance;
const DescriptorSetTypeImpl DescriptorSetTypeImpl::instance;

static const ObjectTypeHandler* typeHandlers[] = {
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_IMAGE, Image, &Device::images>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_IMAGE_VIEW, ImageView, &Device::imageViews>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_SAMPLER, Sampler, &Device::samplers>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_BUFFER, Buffer, &Device::buffers>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_BUFFER_VIEW, BufferView, &Device::bufferViews>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_COMMAND_BUFFER, CommandBuffer, &Device::commandBuffers>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_COMMAND_POOL, CommandPool, &Device::commandPools>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_PIPELINE, Pipeline, &Device::pipes>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_FRAMEBUFFER, Framebuffer, &Device::framebuffers>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_DEVICE_MEMORY, DeviceMemory, &Device::deviceMemories>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_RENDER_PASS, RenderPass, &Device::renderPasses>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_PIPELINE_LAYOUT, PipelineLayout, &Device::pipeLayouts>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, DescriptorSetLayout, &Device::dsLayouts>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_DESCRIPTOR_POOL, DescriptorPool, &Device::dsPools>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_FENCE, Fence, &Device::fences>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_SEMAPHORE, Semaphore, &Device::semaphores>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_EVENT, Event, &Device::events>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_QUERY_POOL, QueryPool, &Device::queryPools>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_SWAPCHAIN_KHR, Swapchain, &Device::swapchains>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_SHADER_MODULE, ShaderModule, &Device::shaderModules>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, DescriptorUpdateTemplate, &Device::dsuTemplates>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, AccelStruct, &Device::accelStructs>::instance,
	&QueueTypeImpl::instance,
	// TODO: we currently don't support this, it causes too many problems.
	// E.g. the returned handles might be destroyed async in another thread,
	// we don't use the device mutex for it. Could likely be fixed
	// &DescriptorSetTypeImpl::instance,
};

const span<const ObjectTypeHandler*> ObjectTypeHandler::handlers = typeHandlers;

Handle* findHandle(Device& dev, VkObjectType objectType, u64 handle, u64& fwdID) {
	// TODO: temporary workaround for descriptor sets, see our wrap optimization
	// is ds.cpp where we don't insert them into maps anymore.
	if(objectType == VK_OBJECT_TYPE_DESCRIPTOR_SET && HandleDesc<VkDescriptorSet>::wrap) {
		auto& ds = get(dev, u64ToHandle<VkDescriptorSet>(handle));
		fwdID = handleToU64(ds.handle);
		return &ds;
	}

	for(auto& handler : ObjectTypeHandler::handlers) {
		if(handler->objectType() == objectType) {
			auto* ptr = handler->find(dev, handle, fwdID);
			if(ptr) {
				return ptr;
			}
		}
	}

	dlg_info("can't find handle {}, type {}", handle, vk::name(objectType));
	return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(
		VkDevice                                    device,
		const VkDebugUtilsObjectNameInfoEXT*        pNameInfo) {
	auto& devd = getDevice(device);
	auto fwd = *pNameInfo;

	{
		std::lock_guard lock(devd.mutex);
		auto* handle = findHandle(devd, pNameInfo->objectType,
			pNameInfo->objectHandle, fwd.objectHandle);
		if(handle) {
			handle->name = pNameInfo->pObjectName;
		}
	}

	if(!devd.dispatch.SetDebugUtilsObjectNameEXT) {
		return VK_SUCCESS;
	}

	return devd.dispatch.SetDebugUtilsObjectNameEXT(device, &fwd);
}

VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(
		VkDevice                                    device,
		const VkDebugUtilsObjectTagInfoEXT*         pTagInfo) {
	auto& devd = getDevice(device);
	auto fwd = *pTagInfo;

	{
		std::lock_guard lock(devd.mutex);
		auto* handle = findHandle(devd, pTagInfo->objectType,
			pTagInfo->objectHandle, fwd.objectHandle);
		if(handle) {
			// TODO: reintroduce (and add UI support).
			// But this time, add a hashMap to device and add it there
			// instead of directly inside the handle for less overhead.
			// auto& data = handle->tags[pTagInfo->tagName];
			// auto ptr = reinterpret_cast<const std::byte*>(pTagInfo->pTag);
			// data = {ptr, ptr + pTagInfo->tagSize};
		}
	}

	if(!devd.dispatch.SetDebugUtilsObjectTagEXT) {
		return VK_SUCCESS;
	}

	return devd.dispatch.SetDebugUtilsObjectTagEXT(device, &fwd);
}

} // namespace vil
