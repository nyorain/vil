#pragma once

#include <fwd.hpp>
#include <handle.hpp>
#include <pipe.hpp>
#include <nytl/mat.hpp>
#include <util/ownbuf.hpp>
#include <variant>

namespace vil {

struct AccelTriangles {
	// TODO: use vec3f. Needs shader adjustments tho
	struct Triangle {
		Vec4f a;
		Vec4f b;
		Vec4f c;
	};

	struct Geometry {
		span<Triangle> triangles; // references the hostVisible buffer
	};

	std::vector<Geometry> geometries;
};

struct AccelAABBs {
	struct Geometry {
		span<VkAabbPositionsKHR> boxes; // references the hostVisible buffer
	};

	std::vector<Geometry> geometries;
};

// NOTE: we do not convert accelerationStructureReference to AccelStruct*
// here by design. We expected acceleration structure builds to happen often
// but this resolving is expensive and only required when showing AccelStruct
// in gui. See docs/accelStruct.md for details.
struct AccelInstances {
	// references the hostVisible buffer
	span<VkAccelerationStructureInstanceKHR> instances;
};

// Ref-counted, can outlive the AccelStruct it originates from.
struct AccelStructState {
	std::atomic<u32> refCount {};
	bool built {}; // whether building has finished.

	// Immutable after creation
	OwnBuffer buffer;
	std::variant<AccelTriangles, AccelAABBs, AccelInstances> data;
};

// AccelerationStructure
struct AccelStruct : SharedDeviceHandle {
	static constexpr auto objectType = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR;

	VkAccelerationStructureKHR handle;
	VkAccelerationStructureTypeKHR type; // can be generic
	VkAccelerationStructureTypeKHR effectiveType; // only relevant when type == generic

	// creation info, immutable.
	// TODO: can buffer get destroyed before AccelStruct?
	//   should unset itself here then.
	//   Or make it an IntrusivePtr
	Buffer* buf {};
	VkDeviceSize offset {};
	VkDeviceSize size {};
	VkDeviceAddress deviceAddress {};

	// The state when all activated and pending submissions are completed.
	// Synced using device mutex.
	IntrusivePtr<AccelStructState> pendingState;

	// The last state this had that has finished building.
	// The same as pendingState, iff pendingState->built == true.
	// Synced using device mutex.
	IntrusivePtr<AccelStructState> lastValid;

	void onApiDestroy();
};

IntrusivePtr<AccelStructState> createState(AccelStruct&,
	const VkAccelerationStructureBuildGeometryInfoKHR& info,
    const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos);

// Assumes that all data pointers are host addresses
// - instancesAreHandles: whether Instances are given via their VkDeviceAddress
//   instead of the vulkan objects.
void copyBuildData(AccelStruct&,
	const VkAccelerationStructureBuildGeometryInfoKHR& info,
    const VkAccelerationStructureBuildRangeInfoKHR* buildRangeInfos,
	bool instancesAreAddresses);

// Returns the AccelStruct located at the given address. The address
// must match exactly.
AccelStruct& accelStructAt(Device& dev, VkDeviceAddress address);
AccelStruct& accelStructAtLocked(Device& dev, VkDeviceAddress address);
AccelStruct* tryAccelStructAtLocked(Device& dev, VkDeviceAddress address);

// Returns a map of all BLASes (that have been built) and their
// pending states at this point in time.
std::unordered_map<VkDeviceAddress, AccelStructStatePtr> captureBLASesLocked(Device& dev);

Mat4f toMat4f(const VkTransformMatrixKHR& src);

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

} // namespace vil
