#include <accelStruct.hpp>
#include <buffer.hpp>
#include <wrap.hpp>
#include <device.hpp>
#include <shader.hpp>
#include <threadContext.hpp>
#include <nytl/matOps.hpp>
#include <util/fmt.hpp>
#include <vkutil/enumString.hpp>
#include <vk/format_utils.h>

namespace vil {

// TODO:
// - handle geometry flags and extra data
// - handle inactive primitives

// util
AccelStruct& accelStructAtLocked(Device& dev, VkDeviceAddress address) {
	auto* accelStruct = tryAccelStructAtLocked(dev, address);
	dlg_assertm(accelStruct,
		"Couldn't find VkAccelerationStructure at address {}", address);
	return *accelStruct;
}

AccelStruct& accelStructAt(Device& dev, VkDeviceAddress address) {
	std::shared_lock lock(dev.mutex);
	return accelStructAtLocked(dev, address);
}

AccelStruct* tryAccelStructAtLocked(Device& dev, VkDeviceAddress address) {
	ZoneScoped;

	assertOwnedOrShared(dev.mutex);
	auto it = dev.accelStructAddresses.find(address);
	if(it == dev.accelStructAddresses.end()) {
		return nullptr;
	}
	return it->second;
}

std::unordered_map<VkDeviceAddress, AccelStructStatePtr> captureBLASesLocked(Device& dev) {
	ZoneScoped;

	// TODO: could be optimized (a bit) by maintaing such a map. Then we would just have to copy it
	assertOwnedOrShared(dev.mutex);

	std::unordered_map<VkDeviceAddress, AccelStructStatePtr> ret;
	for(auto& as : dev.accelStructs.inner) {
		if(as->effectiveType == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR && as->pendingState) {
			ret.emplace(as->deviceAddress, as->pendingState);
		}
	}

	return ret;
}

// building
/*
void writeAABBs(AccelStruct& accelStruct, unsigned id,
		const VkAccelerationStructureGeometryKHR& srcGeom,
		const VkAccelerationStructureBuildRangeInfoKHR& info) {
	dlg_assert(srcGeom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR);
	dlg_assert(accelStruct.effectiveType == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

	auto& src = srcGeom.geometry.aabbs;
	auto& dstAABBs = std::get<AccelAABBs>(accelStruct.data);
	dlg_assert(id < dstAABBs.geometries.size());
	auto& dst = dstAABBs.geometries[id];

	auto ptr = reinterpret_cast<const std::byte*>(src.data.hostAddress);
	ptr += info.primitiveOffset;

	dlg_assert(dst.boxes.size() == info.primitiveCount);
	for(auto i = 0u; i < info.primitiveCount; ++i) {
		auto aabb = reinterpret_cast<const VkAabbPositionsKHR*>(ptr);
		dst.boxes[i] = *aabb;
		ptr += src.stride;
	}
}

void writeTriangles(AccelStruct& accelStruct, unsigned id,
		const VkAccelerationStructureGeometryKHR& srcGeom,
		const VkAccelerationStructureBuildRangeInfoKHR& info) {
	dlg_assert(srcGeom.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR);
	dlg_assert(accelStruct.effectiveType == VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);

	auto& src = srcGeom.geometry.triangles;
	auto& dstTris = std::get<AccelTriangles>(accelStruct.data);
	dlg_assert(id < dstTris.geometries.size());
	auto& dst = dstTris.geometries[id];

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

	auto vertsHaveW = (FormatComponentCount(src.vertexFormat) >= 4);

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

	dlg_assert(dst.triangles.size() == info.primitiveCount);
	if(src.indexType == VK_INDEX_TYPE_NONE_KHR) {
		vertexData += info.primitiveOffset;
		for(auto i = 0u; i < info.primitiveCount; ++i) {
			auto& tri = dst.triangles[i];
			tri.a = readVertex(3 * i + 0);
			tri.b = readVertex(3 * i + 1);
			tri.c = readVertex(3 * i + 2);
		}
	} else {
		auto indexData = reinterpret_cast<const std::byte*>(src.indexData.hostAddress);
		indexData += info.primitiveOffset;
		auto indSize = indexSize(src.indexType);
		for(auto i = 0u; i < info.primitiveCount; ++i) {
			auto& tri = dst.triangles[i];
			auto indData = span{indexData + i * indSize, 3 * indSize};
			tri.a = readVertex(readIndex(src.indexType, indData));
			tri.b = readVertex(readIndex(src.indexType, indData));
			tri.c = readVertex(readIndex(src.indexType, indData));
		}
	}
}

void writeInstances(AccelStruct& accelStruct,
		const VkAccelerationStructureGeometryInstancesDataKHR& src,
		const VkAccelerationStructureBuildRangeInfoKHR& info,
		bool instancesAreAddresses) {
	dlg_assert(accelStruct.effectiveType == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
	auto& dst = std::get<AccelInstances>(accelStruct.data);

	auto ptr = reinterpret_cast<const std::byte*>(src.data.hostAddress);
	ptr += info.primitiveOffset;

	dlg_assertm(info.primitiveCount == dst.instances.size(),
		"{} vs {}", info.primitiveCount, dst.instances.size());

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
			// TODO: we should probably not have this option here and instead
			// do this conversion in CommandHookSubmission.
			// And do this (potentially expensive) operation with lock
			if(addr) {
				auto& ref = accelStructAtLocked(*accelStruct.dev, addr);
				dstIni.accelStruct = &ref;
			} else {
				dstIni.accelStruct = nullptr;
			}
		} else {
			auto vkRef = u64ToHandle<VkAccelerationStructureKHR>(srcIni.accelerationStructureReference);
			if(vkRef) {
				auto& ref = get(*accelStruct.dev, vkRef);
				dstIni.accelStruct = &ref;
			} else {
				dstIni.accelStruct = nullptr;
			}
		}

		dstIni.bindingTableOffset = srcIni.instanceShaderBindingTableRecordOffset;
		dstIni.flags = srcIni.flags;
		dstIni.customIndex = srcIni.instanceCustomIndex;
		dstIni.mask = srcIni.mask;
		dstIni.transform = toMat4f(srcIni.transform);
	}
}
*/

IntrusivePtr<AccelStructState> createState(AccelStruct& accelStruct,
		const VkAccelerationStructureBuildGeometryInfoKHR& info,
		const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos) {
	auto& dev = *accelStruct.dev;

	dlg_assert(info.type != VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR);
	dlg_assert(accelStruct.effectiveType == VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR ||
		info.type == accelStruct.effectiveType);

	accelStruct.effectiveType = info.type;
	if(info.geometryCount == 0u) {
		return {}; // TODO: still return state?
	}

	auto& geom0 = info.pGeometries ? info.pGeometries[0] : *info.ppGeometries[0];

	auto bufSize = 0u;
	for(auto i = 0u; i < info.geometryCount; ++i) {
		auto& geom = info.pGeometries ? info.pGeometries[i] : *info.ppGeometries[i];
		auto& rangeInfo = buildRangeInfos[i];

		if(geom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
			bufSize += rangeInfo.primitiveCount * sizeof(VkAabbPositionsKHR);
		} else if(geom.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
			bufSize += rangeInfo.primitiveCount * sizeof(AccelTriangles::Triangle);
		} else if(geom.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
			bufSize += rangeInfo.primitiveCount * sizeof(VkAccelerationStructureInstanceKHR);
		}
	}

	// Make sure that we at least always create a dummy buffer
	bufSize = std::max(bufSize, 4u);

	auto state = IntrusivePtr<AccelStructState>(new AccelStructState());

	auto usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
		VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	state->buffer.ensure(dev, bufSize, usage);
	auto mapped = state->buffer.map;

	if(geom0.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
		dlg_assert(accelStruct.effectiveType ==
			VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
		auto& aabbs = state->data.emplace<AccelAABBs>();
		aabbs.geometries.resize(info.geometryCount);
	} else if(geom0.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
		dlg_assert(accelStruct.effectiveType ==
			VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
		auto& tris = state->data.emplace<AccelTriangles>();
		tris.geometries.resize(info.geometryCount);
	} else if(geom0.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
		dlg_assert(accelStruct.effectiveType ==
			VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
		auto& instances = state->data.emplace<AccelInstances>();

		dlg_assert(info.geometryCount == 1u);
		auto ptr = reinterpret_cast<VkAccelerationStructureInstanceKHR*>(mapped);
		instances.instances = {ptr, buildRangeInfos[0].primitiveCount};
	} else {
		dlg_error("Invalid VkGeometryTypeKHR: {}", geom0.geometryType);
	}

	auto off = 0u;
	if(geom0.geometryType != VK_GEOMETRY_TYPE_INSTANCES_KHR) {
		for(auto i = 0u; i < info.geometryCount; ++i) {
			auto& geom = info.pGeometries ? info.pGeometries[i] : *info.ppGeometries[i];
			auto& rangeInfo = buildRangeInfos[i];
			dlg_assert(geom.geometryType == geom0.geometryType);

			if(geom.geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
				auto& dst = std::get<AccelAABBs>(state->data).geometries[i];
				auto ptr = reinterpret_cast<VkAabbPositionsKHR*>(mapped + off);
				dst.boxes = {ptr, rangeInfo.primitiveCount};
				off += dst.boxes.size_bytes();
			} else if(geom.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
				auto& dst = std::get<AccelTriangles>(state->data).geometries[i];
				auto ptr = reinterpret_cast<AccelTriangles::Triangle*>(mapped + off);
				dst.triangles = {ptr, rangeInfo.primitiveCount};
				off += dst.triangles.size_bytes();
			} else {
				dlg_error("Invalid VkGeometryTypeKHR: {}", geom0.geometryType);
			}
		}
	}

	return state;
}

Mat4f toMat4f(const VkTransformMatrixKHR& src) {
	Mat<3, 4, float> ret34;
	static_assert(sizeof(ret34) == sizeof(src));
	std::memcpy(&ret34, &src, sizeof(ret34));

	auto ret = Mat4f(ret34);
	ret[3][3] = 1.f;
	return ret;
}

/*
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
			writeAABBs(accelStruct, i, geom, rangeInfo);
		} else if(geom.geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
			writeTriangles(accelStruct, i, geom, rangeInfo);
		} else if(geom.geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
			dlg_assert(i == 0u);
			writeInstances(accelStruct, geom.geometry.instances, rangeInfo,
				instancesAreAddresses);
		} else {
			dlg_fatal("invalid geometry type {}", geom.geometryType);
		}
	}
}
*/

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
	accelStruct.buf = &buf;
	accelStruct.handle = *pAccelerationStructure;
	accelStruct.type = pCreateInfo->type;
	accelStruct.effectiveType = accelStruct.type;
	accelStruct.offset = pCreateInfo->offset;
	accelStruct.size = pCreateInfo->size;

	VkAccelerationStructureDeviceAddressInfoKHR devAddressInfo {};
	devAddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	devAddressInfo.accelerationStructure = accelStruct.handle;
	accelStruct.deviceAddress = dev.dispatch.GetAccelerationStructureDeviceAddressKHR(
		dev.handle, &devAddressInfo);
	dlg_assert(accelStruct.deviceAddress);

	*pAccelerationStructure = castDispatch<VkAccelerationStructureKHR>(accelStruct);
	dev.accelStructs.mustEmplace(std::move(accelStructPtr));

	{
		std::lock_guard lock(dev.mutex);
		auto [_, success] = dev.accelStructAddresses.insert({
			accelStruct.deviceAddress, &accelStruct});
		dlg_assert(success);
	}

	return res;
}

void AccelStruct::onApiDestroy() {
	std::lock_guard lock(dev->mutex);
	dlg_assert(deviceAddress);
	dev->accelStructAddresses.erase(deviceAddress);
}

VKAPI_ATTR void VKAPI_CALL DestroyAccelerationStructureKHR(
		VkDevice                                    device,
		VkAccelerationStructureKHR                  accelerationStructure,
		const VkAllocationCallbacks*                pAllocator) {
	if(!accelerationStructure) {
		return;
	}

	auto& dev = mustMoveUnsetKeepAlive<&Device::keepAliveAccelStructs>(device, accelerationStructure);
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

	// TODO: actually copy data
	dlg_error("TODO: implement host accelerationStructure copying");

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

	// XXX: workaround for validation layer issue
	// src/dst member of pBuildInfo are ignored (per spec) but the
	// validation layer still complains if they are invalid so we simply
	// always unset them. We don't unwrap them since they might contain
	// garbage. This workaround should be removed eventually. We only
	// have it since otherwise it looks like we forgot unwrapping somewhere.
	auto cpy = *pBuildInfo;
	cpy.srcAccelerationStructure = {};
	cpy.dstAccelerationStructure = {};

	return dev.dispatch.GetAccelerationStructureBuildSizesKHR(
		dev.handle, buildType, &cpy, pMaxPrimitiveCounts, pSizeInfo);
}

} // namespace vil
