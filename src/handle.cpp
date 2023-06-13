#include <handle.hpp>
#include <wrap.hpp>
#include <data.hpp>
#include <queue.hpp>
#include <device.hpp>
#include <shader.hpp>
#include <swapchain.hpp>
#include <cb.hpp>
#include <buffer.hpp>
#include <image.hpp>
#include <sync.hpp>
#include <cb.hpp>
#include <pipe.hpp>
#include <ds.hpp>
#include <queryPool.hpp>
#include <rp.hpp>
#include <accelStruct.hpp>
#include <accelStruct.hpp>
#include <vkutil/enumString.hpp>
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

std::string name(const Handle& handle, VkObjectType objectType, bool addType, bool perTypeDefault) {
	const auto hn = name(objectType);

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

		if(objectType == VK_OBJECT_TYPE_IMAGE) {
			name += defaultName(static_cast<const Image&>(handle));
		} else if(objectType == VK_OBJECT_TYPE_IMAGE_VIEW) {
			name += defaultName(static_cast<const ImageView&>(handle));
		} else if(name.empty()) {
			// TODO: not sure if good idea
			name = hn;
			// name = dlg::format("{}{}", std::hex, handleToU64(&handle));
		}
	}

	return name;
}

// case insensitive substring find
// type handlers
bool matchesSearch(Handle& handle, VkObjectType objectType, std::string_view search) {
	if(search.empty()) {
		return true;
	}

	// case-insensitive search
	// TODO: only works on ascii atm, convert to utf32 first?
	// or write method that works on utf8
	auto label = name(handle, objectType);

	return findSubstrCI(label, search) != -1;
}

template<typename... Args>
std::vector<Handle*> findHandles(const std::unordered_map<Args...>& map,
		std::string_view search) {
	std::vector<Handle*> ret;
	for(auto& entry : map) {
		auto& handle = *entry.second;
		if(!matchesSearch(handle, handle.objectType, search)) {
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
		if(!matchesSearch(handle, handle.objectType, search)) {
			continue;
		}

		ret.push_back(&handle);
	}

	std::sort(ret.begin(), ret.end());
	return ret;
}

void ResourceVisitor::visit(ComputePipeline& p) {
	visit(static_cast<Pipeline&>(p));
}
void ResourceVisitor::visit(GraphicsPipeline& p) {
	visit(static_cast<Pipeline&>(p));
}
void ResourceVisitor::visit(RayTracingPipeline& p) {
	visit(static_cast<Pipeline&>(p));
}

template<VkObjectType OT, typename HT, auto DevMapPtr>
struct ObjectTypeMapImpl : ObjectTypeHandler {
	static const ObjectTypeMapImpl instance;

	VkObjectType objectType() const override { return OT; }
	Handle* find(Device& dev, u64 id, u64& fwdID) const override {
		assertOwned(dev.mutex);

		using VKHT = decltype(std::declval<HT>().handle);
		auto vkht = u64ToHandle<VKHT>(id);

		auto& handle = getLocked(dev, vkht);
		fwdID = handleToU64(handle.handle);

		return &handle;
	}
	std::vector<Handle*> resources(Device& dev, std::string_view search) const override {
		return findHandles((dev.*DevMapPtr).inner, search);
	}
	void visit(ResourceVisitor& visitor, Handle& handle) const override {
		return visitor.visit(static_cast<HT&>(handle));
	}
};

struct DescriptorSetTypeImpl : ObjectTypeHandler {
	static const DescriptorSetTypeImpl instance;

	// when we wrap descriptor sets, we don't insert them into the
	// device map

	VkObjectType objectType() const override { return VK_OBJECT_TYPE_DESCRIPTOR_SET; }

	// NOTE: must only be called while api guarantees that the handle
	// stays valid, ds could be destroyed in another thread otherwise (lifetime
	// not bound to device mutex as an optimization)
	Handle* find(Device& dev, u64 id, u64& fwdID) const override {
		assertOwned(dev.mutex);

		auto vkds = u64ToHandle<VkDescriptorSet>(id);
		auto& handle = getLocked(dev, vkds);
		fwdID = handleToU64(handle.handle);

		return &handle;
	}
	std::vector<Handle*> resources(Device& dev, std::string_view search) const override {
		(void) dev;
		(void) search;
		dlg_error("Enumerating DescriptorSets not supported, should not be called");
		return {};
	}
	void visit(ResourceVisitor& visitor, Handle& handle) const override {
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
			if(queue->createdByUs || !matchesSearch(*queue, objectType(), search)) {
				continue;
			}

			ret.push_back(queue.get());
		}

		return ret;
	}
	void visit(ResourceVisitor& visitor, Handle& handle) const override {
		return visitor.visit(static_cast<Queue&>(handle));
	}
};

struct PipelineTypeImpl : ObjectTypeMapImpl<VK_OBJECT_TYPE_PIPELINE, Pipeline, &Device::pipes> {
	static const PipelineTypeImpl instance;

	void visit(ResourceVisitor& visitor, Handle& handle) const override {
		auto& pipe = static_cast<Pipeline&>(handle);
		switch(pipe.type) {
			case VK_PIPELINE_BIND_POINT_GRAPHICS:
				visitor.visit(static_cast<GraphicsPipeline&>(pipe));
				break;
			case VK_PIPELINE_BIND_POINT_COMPUTE:
				visitor.visit(static_cast<ComputePipeline&>(pipe));
				break;
			case VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR:
				visitor.visit(static_cast<RayTracingPipeline&>(pipe));
				break;
			default:
				dlg_error("unimplemented");
				visitor.visit(pipe);
				break;
		}
	}
};

template<VkObjectType OT, typename HT, auto DevMapPtr>
const ObjectTypeMapImpl<OT, HT, DevMapPtr> ObjectTypeMapImpl<OT, HT, DevMapPtr>::instance;

const QueueTypeImpl QueueTypeImpl::instance;
const DescriptorSetTypeImpl DescriptorSetTypeImpl::instance;
const PipelineTypeImpl PipelineTypeImpl::instance;

static const ObjectTypeHandler* typeHandlers[] = {
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_IMAGE, Image, &Device::images>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_IMAGE_VIEW, ImageView, &Device::imageViews>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_SAMPLER, Sampler, &Device::samplers>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_BUFFER, Buffer, &Device::buffers>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_BUFFER_VIEW, BufferView, &Device::bufferViews>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_COMMAND_BUFFER, CommandBuffer, &Device::commandBuffers>::instance,
	&ObjectTypeMapImpl<VK_OBJECT_TYPE_COMMAND_POOL, CommandPool, &Device::commandPools>::instance,
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
	&PipelineTypeImpl::instance,
	&QueueTypeImpl::instance,
	// NOTE: this one is special, it does not support all operations.
	// Caller must handle this on their side.
	&DescriptorSetTypeImpl::instance,
};

const span<const ObjectTypeHandler*> ObjectTypeHandler::handlers = typeHandlers;

const ObjectTypeHandler* ObjectTypeHandler::handler(VkObjectType type) {
	for(auto& handler : ObjectTypeHandler::handlers) {
		if(handler->objectType() == type) {
			return handler;
		}
	}

	dlg_warn("Requested object type handler for unknown type {} ({})",
		vk::name(type), u64(type));
	return nullptr;
}

Handle* findHandle(Device& dev, VkObjectType objectType, u64 handle, u64& fwdID) {
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
