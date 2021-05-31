#include <rt.hpp>
#include <buffer.hpp>
#include <device.hpp>
#include <threadContext.hpp>

namespace vil {

VKAPI_ATTR VkResult VKAPI_CALL CreateAccelerationStructureKHR(
		VkDevice                                    device,
		const VkAccelerationStructureCreateInfoKHR* pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkAccelerationStructureKHR*                 pAccelerationStructure) {
	auto& buf = get(device, pCreateInfo->buffer);
	auto& dev = *buf.dev;

	auto fwd = *pCreateInfo;
	fwd.buffer = buf.handle;

	auto res = dev.dispatch.CreateAccelerationStructureKHR(dev.handle, &fwd,
		pAllocator, pAccelerationStructure);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto accelStructPtr = IntrusivePtr<AccelStruct>(new AccelStruct());
	auto& accelStruct = *accelStructPtr;
	accelStruct.dev = &dev;
	accelStruct.objectType = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR;
	accelStruct.buf = &buf;
	accelStruct.handle = *pAccelerationStructure;
	accelStruct.type = pCreateInfo->type;
	accelStruct.offset = pCreateInfo->offset;
	accelStruct.size = pCreateInfo->size;

	*pAccelerationStructure = castDispatch<VkAccelerationStructureKHR>(accelStruct);
	dev.accelStructs.mustEmplace(*pAccelerationStructure, std::move(accelStructPtr));

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyAccelerationStructureKHR(
		VkDevice                                    device,
		VkAccelerationStructureKHR                  accelerationStructure,
		const VkAllocationCallbacks*                pAllocator) {
	if(!accelerationStructure) {
		return;
	}

	auto& dev = getDevice(device);
	IntrusivePtr<AccelStruct> ptr;

	{
		auto lock = std::lock_guard(dev.mutex);
		ptr = dev.accelStructs.mustMoveLocked(accelerationStructure);
		accelerationStructure = ptr->handle;
		ptr->handle = {};
	}

	dev.dispatch.DestroyAccelerationStructureKHR(dev.handle, accelerationStructure, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL BuildAccelerationStructuresKHR(
		VkDevice                                    device,
		VkDeferredOperationKHR                      deferredOperation,
		uint32_t                                    infoCount,
		const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
		const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos) {
	auto& dev = getDevice(device);

	// TODO: store data
	dlg_error("TODO: implement host accelerationStructure building");

	ThreadMemScope memScope;
	auto infos = memScope.copy(pInfos, infoCount);
	for(auto& info : infos) {
		if(info.srcAccelerationStructure) {
			auto& src = get(dev, info.srcAccelerationStructure);
			info.srcAccelerationStructure = src.handle;
		}

		auto& dst = get(dev, info.dstAccelerationStructure);
		info.dstAccelerationStructure = dst.handle;
	}

	return dev.dispatch.BuildAccelerationStructuresKHR(dev.handle, deferredOperation,
		infoCount, infos.data(), ppBuildRangeInfos);
}

VKAPI_ATTR VkResult VKAPI_CALL CopyAccelerationStructureKHR(
		VkDevice                                    device,
		VkDeferredOperationKHR                      deferredOperation,
		const VkCopyAccelerationStructureInfoKHR*   pInfo) {
	auto& src = get(device, pInfo->src);
	auto& dev = *src.dev;
	auto& dst = get(dev, pInfo->src);

	auto fwd = *pInfo;
	fwd.src = src.handle;
	fwd.dst = dst.handle;

	return dev.dispatch.CopyAccelerationStructureKHR(dev.handle, deferredOperation, &fwd);
}

VKAPI_ATTR VkResult VKAPI_CALL CopyAccelerationStructureToMemoryKHR(
		VkDevice                                    device,
		VkDeferredOperationKHR                      deferredOperation,
		const VkCopyAccelerationStructureToMemoryInfoKHR* pInfo) {
	auto& src = get(device, pInfo->src);
	auto& dev = *src.dev;

	auto fwd = *pInfo;
	fwd.src = src.handle;

	return dev.dispatch.CopyAccelerationStructureToMemoryKHR(dev.handle, deferredOperation, &fwd);
}

VKAPI_ATTR VkResult VKAPI_CALL CopyMemoryToAccelerationStructureKHR(
		VkDevice                                    device,
		VkDeferredOperationKHR                      deferredOperation,
		const VkCopyMemoryToAccelerationStructureInfoKHR* pInfo) {
	auto& dst = get(device, pInfo->dst);
	auto& dev = *dst.dev;

	auto fwd = *pInfo;
	fwd.dst = dst.handle;

	return dev.dispatch.CopyMemoryToAccelerationStructureKHR(dev.handle, deferredOperation, &fwd);
}

VKAPI_ATTR VkResult VKAPI_CALL WriteAccelerationStructuresPropertiesKHR(
		VkDevice                                    device,
		uint32_t                                    accelerationStructureCount,
		const VkAccelerationStructureKHR*           pAccelerationStructures,
		VkQueryType                                 queryType,
		size_t                                      dataSize,
		void*                                       pData,
		size_t                                      stride) {
	auto& dev = getDevice(device);

	ThreadMemScope memScope;
	auto handles = memScope.alloc<VkAccelerationStructureKHR>(accelerationStructureCount);

	for(auto i = 0u; i < accelerationStructureCount; ++i) {
		auto& accelStruct = get(dev, pAccelerationStructures[i]);
		handles[i] = accelStruct.handle;
	}

	return dev.dispatch.WriteAccelerationStructuresPropertiesKHR(dev.handle,
		u32(handles.size()), handles.data(), queryType, dataSize,
		pData, stride);
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetAccelerationStructureDeviceAddressKHR(
		VkDevice                                    device,
		const VkAccelerationStructureDeviceAddressInfoKHR* pInfo) {
	auto& accelStruct = get(device, pInfo->accelerationStructure);
	auto& dev = *accelStruct.dev;

	auto fwd = *pInfo;
	fwd.accelerationStructure = accelStruct.handle;

	return dev.dispatch.GetAccelerationStructureDeviceAddressKHR(dev.handle, &fwd);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceAccelerationStructureCompatibilityKHR(
		VkDevice                                    device,
		const VkAccelerationStructureVersionInfoKHR* pVersionInfo,
		VkAccelerationStructureCompatibilityKHR*    pCompatibility) {
	auto& dev = getDevice(device);
	return dev.dispatch.GetDeviceAccelerationStructureCompatibilityKHR(
		dev.handle, pVersionInfo, pCompatibility);
}

VKAPI_ATTR void VKAPI_CALL GetAccelerationStructureBuildSizesKHR(
		VkDevice                                    device,
		VkAccelerationStructureBuildTypeKHR         buildType,
		const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo,
		const uint32_t*                             pMaxPrimitiveCounts,
		VkAccelerationStructureBuildSizesInfoKHR*   pSizeInfo) {
	auto& dev = getDevice(device);
	// we don't have to unwrap src/dst member of pBuildInfo since
	// they are ignored (per spec)
	return dev.dispatch.GetAccelerationStructureBuildSizesKHR(
		dev.handle, buildType, pBuildInfo, pMaxPrimitiveCounts, pSizeInfo);
}

// VK_KHR_ray_tracing_pipeline
VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(
		VkDevice                                    device,
		VkDeferredOperationKHR                      deferredOperation,
		VkPipelineCache                             pipelineCache,
		uint32_t                                    createInfoCount,
		const VkRayTracingPipelineCreateInfoKHR*    pCreateInfos,
		const VkAllocationCallbacks*                pAllocator,
		VkPipeline*                                 pPipelines) {
	dlg_error("TODO: implement CreateRayTracingPipelinesKHR");
	auto& dev = getDevice(device);
	return dev.dispatch.CreateRayTracingPipelinesKHR(dev.handle,
		deferredOperation, pipelineCache, createInfoCount, pCreateInfos,
		pAllocator, pPipelines);
}

VKAPI_ATTR VkResult VKAPI_CALL GetRayTracingCaptureReplayShaderGroupHandlesKHR(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		uint32_t                                    firstGroup,
		uint32_t                                    groupCount,
		size_t                                      dataSize,
		void*                                       pData) {
	auto& dev = getDevice(device);
	return dev.dispatch.GetRayTracingCaptureReplayShaderGroupHandlesKHR(dev.handle,
		pipeline, firstGroup, groupCount, dataSize, pData);
}

VKAPI_ATTR VkDeviceSize VKAPI_CALL GetRayTracingShaderGroupStackSizeKHR(
		VkDevice                                    device,
		VkPipeline                                  pipeline,
		uint32_t                                    group,
		VkShaderGroupShaderKHR                      groupShader) {
	auto& dev = getDevice(device);
	return dev.dispatch.GetRayTracingShaderGroupStackSizeKHR(
		dev.handle, pipeline, group, groupShader);
}

} // namespace vil
