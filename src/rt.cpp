#include <rt.hpp>
#include <buffer.hpp>
#include <device.hpp>
#include <shader.hpp>
#include <threadContext.hpp>
#include <util/matOps.hpp>
#include <vk/format_utils.h>

namespace vil {

// util
AccelStruct& accelStructAt(Device& dev, VkDeviceAddress address) {
	auto lock = std::lock_guard(dev.mutex);
	auto it = dev.accelStructAddresses.find(address);
	dlg_assert(it != dev.accelStructAddresses.end());
	return nonNull(it->second);
}

// building
Mat4f toMat4f(const VkTransformMatrixKHR& src) {
	Mat<3, 4, float> ret34;
	static_assert(sizeof(ret34) == sizeof(src));
	std::memcpy(&ret34, &src, sizeof(ret34));

	auto ret = Mat4f(ret34);
	ret[3][3] = 1.f;
	return ret;
}

void writeAABBs(AccelStruct& accelStruct,
		const VkAccelerationStructureGeometryAabbsDataKHR& src,
		const VkAccelerationStructureBuildRangeInfoKHR& info) {
	(void) accelStruct;
	(void) src;
	(void) info;
	/*
	dlg_assert(accelStruct.effectiveType = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
	auto& dst = std::get<AccelAABBs>(accelStruct.data);

	auto ptr = reinterpret_cast<const std::byte*>(src.data.hostAddress);
	ptr += info.primitiveOffset;

	dst.boxes.resize(info.primitiveCount);
	for(auto i = 0u; i < info.primitiveCount; ++i) {
		auto aabb = reinterpret_cast<const VkAabbPositionsKHR*>(ptr);
		dst.boxes[i] = *aabb;
		ptr += src.stride;
	}
	*/
}

void writeTriangles(AccelStruct& accelStruct,
		const VkAccelerationStructureGeometryTrianglesDataKHR& src,
		const VkAccelerationStructureBuildRangeInfoKHR& info) {
	(void) accelStruct;
	(void) src;
	(void) info;
	/*
	dlg_assert(accelStruct.effectiveType = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
	auto& dst = std::get<AccelTriangles>(accelStruct.data);

	auto vertexData = reinterpret_cast<const std::byte*>(src.vertexData.hostAddress);
	vertexData += info.firstVertex * src.vertexStride;

	// retrieve transform
	Mat4f transform = nytl::identity<4, float>();
	if(src.transformData.hostAddress) {
		auto* ptr = reinterpret_cast<const std::byte*>(src.transformData.hostAddress);
		ptr += info.transformOffset;

		auto* src = reinterpret_cast<const VkTransformMatrixKHR*>(ptr);
		transform = toMat4f(*src);
	}

	auto vertsHaveW = (FormatChannelCount(src.vertexFormat) >= 4);

	auto readVertex = [&](u32 index) {
		dlg_assert(index <= src.maxVertex);

		auto ptr = vertexData + index * src.vertexStride;
		auto data = span{ptr, src.vertexStride};
		auto vert = Vec4f(read(src.vertexFormat, data));
		if(!vertsHaveW) {
			vert[3] = 1.f;
		}

		if(src.transformData.hostAddress) {
			vert = transform * vert;
		}

		return vert;
	};

	if(src.indexType == VK_INDEX_TYPE_NONE_KHR) {
		vertexData += info.primitiveOffset;
		for(auto i = 0u; i < info.primitiveCount; ++i) {
			auto& tri = dst.triangles.emplace_back();
			tri.a = readVertex(3 * i + 0);
			tri.b = readVertex(3 * i + 1);
			tri.c = readVertex(3 * i + 2);
		}
	} else {
		auto indexData = reinterpret_cast<const std::byte*>(src.indexData.hostAddress);
		indexData += info.primitiveOffset;
		auto indSize = indexSize(src.indexType);
		for(auto i = 0u; i < info.primitiveCount; ++i) {
			auto& tri = dst.triangles.emplace_back();
			auto indData = span{indexData + i * indSize, 3 * indSize};
			tri.a = readVertex(readIndex(src.indexType, indData));
			tri.b = readVertex(readIndex(src.indexType, indData));
			tri.c = readVertex(readIndex(src.indexType, indData));
		}
	}
	*/
}

void writeInstances(AccelStruct& accelStruct,
		const VkAccelerationStructureGeometryInstancesDataKHR& src,
		const VkAccelerationStructureBuildRangeInfoKHR& info,
		bool instancesAreAddresses) {
	dlg_assert(accelStruct.effectiveType = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
	auto& dst = std::get<AccelInstances>(accelStruct.data);

	auto ptr = reinterpret_cast<const std::byte*>(src.data.hostAddress);
	ptr += info.primitiveOffset;

	for(auto i = 0u; i < info.primitiveCount; ++i) {
		const VkAccelerationStructureInstanceKHR* pSrcIni;
		if(src.arrayOfPointers) {
			auto ptrData = reinterpret_cast<const VkDeviceOrHostAddressConstKHR*>(ptr) + i;
			pSrcIni = reinterpret_cast<const VkAccelerationStructureInstanceKHR*>(ptrData->hostAddress);
		} else {
			pSrcIni = reinterpret_cast<const VkAccelerationStructureInstanceKHR*>(ptr) + i;
		}

		auto& srcIni = *pSrcIni;

		dlg_assert(i < dst.instances.size());
		auto& dstIni = dst.instances[i];

		if(instancesAreAddresses) {
			auto addr = VkDeviceAddress(srcIni.accelerationStructureReference);
			auto& ref = accelStructAt(*accelStruct.dev, addr);
			dstIni.accelStruct = &ref;
		} else {
			auto vkRef = u64ToHandle<VkAccelerationStructureKHR>(srcIni.accelerationStructureReference);
			auto& ref = get(*accelStruct.dev, vkRef);
			dstIni.accelStruct = &ref;
		}

		dstIni.bindingTableOffset = srcIni.instanceShaderBindingTableRecordOffset;
		dstIni.flags = srcIni.flags;
		dstIni.customIndex = srcIni.instanceCustomIndex;
		dstIni.mask = srcIni.mask;
		dstIni.transform = toMat4f(srcIni.transform);
	}
}

void initBufs(AccelStruct& accelStruct,
		const VkAccelerationStructureBuildGeometryInfoKHR& info,
		const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos) {
	auto& dev = *accelStruct.dev;

	accelStruct.effectiveType = accelStruct.type;
	if(accelStruct.type == VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR) {
		accelStruct.effectiveType = info.type;
	}

	if(info.geometryCount == 0u) {
		return;
	}

	auto& geom0 = info.pGeometries ? info.pGeometries[0] : *info.ppGeometries[0];
	accelStruct.geometryType = geom0.geometryType;

	auto bufSize = 0u;
	for(auto i = 0u; i < info.geometryCount; ++i) {
		auto& geom = info.pGeometries ? info.pGeometries[i] : *info.ppGeometries[i];
		auto& rangeInfo = buildRangeInfos[i];

		if(geom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
			bufSize += rangeInfo.primitiveCount * sizeof(VkAabbPositionsKHR);
		} else if(geom.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
			bufSize += rangeInfo.primitiveCount * sizeof(AccelTriangles::Triangle);
		}
	}

	std::byte* mapped {};
	auto usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	if(geom0.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
		dlg_assert(accelStruct.effectiveType =
			VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
		auto& aabbs = accelStruct.data.emplace<AccelAABBs>();
		aabbs.geometries.resize(info.geometryCount);
		aabbs.buffer.ensure(dev, bufSize, usage);
		mapped = aabbs.buffer.map;
	} else if(geom0.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
		dlg_assert(accelStruct.effectiveType =
			VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
		auto& tris = accelStruct.data.emplace<AccelTriangles>();
		tris.geometries.resize(info.geometryCount);
		tris.buffer.ensure(dev, bufSize, usage);
		mapped = tris.buffer.map;
	} else if(geom0.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
		dlg_assert(accelStruct.effectiveType =
			VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
		accelStruct.data = AccelInstances {};
		dlg_assert(bufSize == 0u);
		dlg_assert(info.geometryCount == 1u);
	}

	auto off = 0u;
	for(auto i = 0u; i < info.geometryCount; ++i) {
		auto& geom = info.pGeometries ? info.pGeometries[i] : *info.ppGeometries[i];
		auto& rangeInfo = buildRangeInfos[i];

		if(geom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
			auto& dst = std::get<AccelAABBs>(accelStruct.data).geometries[i];
			auto ptr = reinterpret_cast<VkAabbPositionsKHR*>(mapped + off);
			dst.boxes = {ptr, rangeInfo.primitiveCount};
			off += dst.boxes.size_bytes();
		} else if(geom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
			auto& dst = std::get<AccelTriangles>(accelStruct.data).geometries[i];
			auto ptr = reinterpret_cast<AccelTriangles::Triangle*>(mapped + off);
			dst.triangles = {ptr, rangeInfo.primitiveCount};
			off += dst.triangles.size_bytes();
		}
	}

	dlg_assert(off == bufSize);
}

void copyBuildData(AccelStruct& accelStruct,
		const VkAccelerationStructureBuildGeometryInfoKHR& info,
		const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos,
		bool instancesAreAddresses) {
	dlg_assertm(accelStruct.geometryType != VK_GEOMETRY_TYPE_MAX_ENUM_KHR,
		"Acceleration struct was never initialized");

	for(auto i = 0u; i < info.geometryCount; ++i) {
		auto& geom = info.pGeometries ? info.pGeometries[i] : *info.ppGeometries[i];
		auto& rangeInfo = buildRangeInfos[i];

		dlg_assert(accelStruct.geometryType == geom.geometryType);
		if(geom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
			writeAABBs(accelStruct, geom.geometry.aabbs, rangeInfo);
		} else if(geom.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
			writeTriangles(accelStruct, geom.geometry.triangles, rangeInfo);
		} else if(geom.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
			dlg_assert(i == 0u);
			writeInstances(accelStruct, geom.geometry.instances, rangeInfo,
				instancesAreAddresses);
		} else {
			dlg_fatal("unreachable; invalid geometry type {}", geom.geometryType);
		}
	}
}

// api
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

	VkAccelerationStructureDeviceAddressInfoKHR devAddressInfo {};
	devAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	devAddressInfo.accelerationStructure = accelStruct.handle;
	accelStruct.deviceAddress = dev.dispatch.GetAccelerationStructureDeviceAddressKHR(
		dev.handle, &devAddressInfo);
	dlg_assert(accelStruct.deviceAddress);

	*pAccelerationStructure = castDispatch<VkAccelerationStructureKHR>(accelStruct);
	dev.accelStructs.mustEmplace(*pAccelerationStructure, std::move(accelStructPtr));

	{
		std::lock_guard lock(dev.mutex);
		auto [_, success] = dev.accelStructAddresses.insert({
			accelStruct.deviceAddress, &accelStruct});
		dlg_assert(success);
	}

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

		dlg_assert(ptr->deviceAddress);
		dev.accelStructAddresses.erase(ptr->deviceAddress);

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
	ZoneScoped;
	auto& dev = getDevice(device);

	ThreadMemScope memScope;
	auto ncis = memScope.copy(pCreateInfos, createInfoCount);
	for(auto& nci : ncis) {
		nci.layout = get(device, nci.layout).handle;
		if(nci.basePipelineHandle) {
			auto& basePipe = get(dev, nci.basePipelineHandle);
			nci.basePipelineHandle = basePipe.handle;
		}

		if(nci.pLibraryInfo) {
			auto& copy = memScope.alloc<VkPipelineLibraryCreateInfoKHR>(1)[0];
			copy = *nci.pLibraryInfo;

			auto libHandles = memScope.alloc<VkPipeline>(copy.libraryCount);
			for(auto i = 0u; i < copy.libraryCount; ++i) {
				auto& lib = get(dev, copy.pLibraries[i]);
				libHandles[i] = lib.handle;
			}

			copy.pLibraries = libHandles.data();
		}
	}

	{
		ZoneScopedN("dispatch");
		auto res = dev.dispatch.CreateRayTracingPipelinesKHR(dev.handle,
			deferredOperation, pipelineCache, createInfoCount, ncis.data(),
			pAllocator, pPipelines);
		if(res != VK_SUCCESS) {
			return res;
		}
	}

	for(auto i = 0u; i < createInfoCount; ++i) {
		dlg_assert(pPipelines[i]);
		auto& ci = pCreateInfos[i];

		auto pipePtr = std::make_unique<RayTracingPipeline>();
		auto& pipe = *pipePtr;
		pipe.objectType = VK_OBJECT_TYPE_PIPELINE;
		pipe.type = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
		pipe.dev = &dev;
		pipe.handle = pPipelines[i];
		pipe.layout = getPtr(dev, ci.layout);

		if(ci.pDynamicState) {
			pipe.dynamicState = {
				ci.pDynamicState->pDynamicStates,
				ci.pDynamicState->pDynamicStates + ci.pDynamicState->dynamicStateCount
			};
		}

		for(auto i = 0u; i < ci.stageCount; ++i) {
			pipe.stages.emplace_back(dev, ci.pStages[i]);
		}

		for(auto i = 0u; i < ci.groupCount; ++i) {
			auto& src = ci.pGroups[i];
			auto& dst = pipe.groups.emplace_back();

			dst.anyHit = src.anyHitShader;
			dst.closestHit = src.closestHitShader;
			dst.general = src.generalShader;
			dst.intersection = src.intersectionShader;
			dst.type = src.type;
		}

		pPipelines[i] = castDispatch<VkPipeline>(static_cast<Pipeline&>(pipe));
		dev.pipes.mustEmplace(pPipelines[i], std::move(pipePtr));
	}

	return VK_SUCCESS;
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
