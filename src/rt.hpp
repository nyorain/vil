#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <pipe.hpp>
#include <util/mat.hpp>
#include <util/ownbuf.hpp>
#include <variant>

namespace vil {

struct AccelTriangles {
	struct Triangle {
		Vec4f a;
		Vec4f b;
		Vec4f c;
	};

	OwnBuffer buffer;
	span<Triangle> triangles; // references the hostVisible buffer
};

struct AccelAABBs {
	OwnBuffer buffer;
	span<VkAabbPositionsKHR> boxes; // references the hostVisible buffer
};

struct AccelInstances {
	struct Instance {
		AccelStruct* accelStruct;
		Mat4f transform;
		u32 customIndex;
		u32 bindingTableOffset;
		VkGeometryInstanceFlagsKHR flags;
		u8 mask;
	};

	std::vector<Instance> instances;
};

// AccelerationStructure
struct AccelStruct : DeviceHandle {
	VkAccelerationStructureKHR handle;
	VkAccelerationStructureTypeKHR type; // can be generic
	VkAccelerationStructureTypeKHR effectiveType; // only relevant when type == generic

	// creation info
	Buffer* buf {};
	VkDeviceSize offset {};
	VkDeviceSize size {};

	std::atomic<u32> refCount {};

	// geometry info
	VkGeometryTypeKHR geometryType {VK_GEOMETRY_TYPE_MAX_ENUM_KHR};
	std::variant<AccelTriangles, AccelAABBs, AccelInstances> data;
};

struct RayTracingPipeline : Pipeline {
	struct Group {
		VkRayTracingShaderGroupTypeKHR type;
		u32 general;
		u32 closestHit;
		u32 anyHit;
		u32 intersection;
	};

	std::vector<PipelineShaderStage> stages;
	std::vector<Group> groups;
	std::unordered_set<VkDynamicState> dynamicState;
};

// Assumes that all data pointers are host addresses
void copyBuildData(AccelStruct&,
	const VkAccelerationStructureBuildGeometryInfoKHR& info,
    const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos);

// VK_KHR_acceleration_structure
VKAPI_ATTR VkResult VKAPI_CALL CreateAccelerationStructureKHR(
    VkDevice                                    device,
    const VkAccelerationStructureCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkAccelerationStructureKHR*                 pAccelerationStructure);

VKAPI_ATTR void VKAPI_CALL DestroyAccelerationStructureKHR(
    VkDevice                                    device,
    VkAccelerationStructureKHR                  accelerationStructure,
    const VkAllocationCallbacks*                pAllocator);

VKAPI_ATTR VkResult VKAPI_CALL BuildAccelerationStructuresKHR(
    VkDevice                                    device,
    VkDeferredOperationKHR                      deferredOperation,
    uint32_t                                    infoCount,
    const VkAccelerationStructureBuildGeometryInfoKHR* pInfos,
    const VkAccelerationStructureBuildRangeInfoKHR* const* ppBuildRangeInfos);

VKAPI_ATTR VkResult VKAPI_CALL CopyAccelerationStructureKHR(
    VkDevice                                    device,
    VkDeferredOperationKHR                      deferredOperation,
    const VkCopyAccelerationStructureInfoKHR*   pInfo);

VKAPI_ATTR VkResult VKAPI_CALL CopyAccelerationStructureToMemoryKHR(
    VkDevice                                    device,
    VkDeferredOperationKHR                      deferredOperation,
    const VkCopyAccelerationStructureToMemoryInfoKHR* pInfo);

VKAPI_ATTR VkResult VKAPI_CALL CopyMemoryToAccelerationStructureKHR(
    VkDevice                                    device,
    VkDeferredOperationKHR                      deferredOperation,
    const VkCopyMemoryToAccelerationStructureInfoKHR* pInfo);

VKAPI_ATTR VkResult VKAPI_CALL WriteAccelerationStructuresPropertiesKHR(
    VkDevice                                    device,
    uint32_t                                    accelerationStructureCount,
    const VkAccelerationStructureKHR*           pAccelerationStructures,
    VkQueryType                                 queryType,
    size_t                                      dataSize,
    void*                                       pData,
    size_t                                      stride);

VKAPI_ATTR VkDeviceAddress VKAPI_CALL GetAccelerationStructureDeviceAddressKHR(
    VkDevice                                    device,
    const VkAccelerationStructureDeviceAddressInfoKHR* pInfo);

VKAPI_ATTR void VKAPI_CALL GetDeviceAccelerationStructureCompatibilityKHR(
    VkDevice                                    device,
    const VkAccelerationStructureVersionInfoKHR* pVersionInfo,
    VkAccelerationStructureCompatibilityKHR*    pCompatibility);

VKAPI_ATTR void VKAPI_CALL GetAccelerationStructureBuildSizesKHR(
    VkDevice                                    device,
    VkAccelerationStructureBuildTypeKHR         buildType,
    const VkAccelerationStructureBuildGeometryInfoKHR* pBuildInfo,
    const uint32_t*                             pMaxPrimitiveCounts,
    VkAccelerationStructureBuildSizesInfoKHR*   pSizeInfo);

// VK_KHR_ray_tracing_pipeline
VKAPI_ATTR VkResult VKAPI_CALL CreateRayTracingPipelinesKHR(
    VkDevice                                    device,
    VkDeferredOperationKHR                      deferredOperation,
    VkPipelineCache                             pipelineCache,
    uint32_t                                    createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR*    pCreateInfos,
    const VkAllocationCallbacks*                pAllocator,
    VkPipeline*                                 pPipelines);

VKAPI_ATTR VkResult VKAPI_CALL GetRayTracingCaptureReplayShaderGroupHandlesKHR(
    VkDevice                                    device,
    VkPipeline                                  pipeline,
    uint32_t                                    firstGroup,
    uint32_t                                    groupCount,
    size_t                                      dataSize,
    void*                                       pData);

VKAPI_ATTR VkDeviceSize VKAPI_CALL GetRayTracingShaderGroupStackSizeKHR(
    VkDevice                                    device,
    VkPipeline                                  pipeline,
    uint32_t                                    group,
    VkShaderGroupShaderKHR                      groupShader);

} // namespace vil
