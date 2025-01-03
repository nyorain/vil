#pragma once

#include <fwd.hpp>
#include <vkutil/bufferSpan.hpp>
#include <threadContext.hpp>

namespace vil::vku {

constexpr VkImageLayout invalidImageLayout = VK_IMAGE_LAYOUT_MAX_ENUM;

struct SyncScope {
	VkPipelineStageFlags stages {};
	VkAccessFlags access {};
	VkImageLayout layout {invalidImageLayout};

	static inline SyncScope discard(
			VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) {
		return {
			srcStage, {},
			VK_IMAGE_LAYOUT_UNDEFINED,
		};
	}

	// NOTE: in many cases we could do better in theory, i.e. not all
	// of memoryRead/memoryWrite is needed. But tracking exactly
	// which resources might have accessed the memory in which way before
	// an aliased resource gets activated is hard, just syncing
	// overconservative here seems like a good tradeoff
	static inline SyncScope aliasDiscard() {
		return {
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED,
		};
	}

	static inline SyncScope fragmentRead() {
		return {
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
	}
	static inline SyncScope computeRead() {
		return {
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
	}
	static inline SyncScope computeReadStorage() {
		return {
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_IMAGE_LAYOUT_GENERAL,
		};
	}
	static inline SyncScope computeReadWrite() {
		return {
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			VK_IMAGE_LAYOUT_GENERAL,
		};
	}
	static inline SyncScope computeWrite() {
		return {
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_WRITE_BIT,
			VK_IMAGE_LAYOUT_GENERAL,
		};
	}
	static inline SyncScope vertexRead() {
		return {
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
	}
	static inline SyncScope allShaderRead() {
		return {
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_ACCESS_SHADER_READ_BIT,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
	}
	static inline SyncScope transferRead() {
		return {
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_READ_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		};
	}
	static inline SyncScope transferWrite() {
		return {
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		};
	}

	// buffer-only
	static inline SyncScope vertexInput() {
		return {
			VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
			VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
			invalidImageLayout
		};
	}
	static inline SyncScope readIndirectCommand() { // draw/dispatch indirect
		return {
			VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
			VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
			invalidImageLayout,
		};
	}
	static inline SyncScope computeUniformRead() {
		return {
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_UNIFORM_READ_BIT,
			invalidImageLayout,
		};
	}
	static inline SyncScope fragmentUniformRead() {
		return {
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_UNIFORM_READ_BIT,
			invalidImageLayout,
		};
	}
	static inline SyncScope vertexUniformRead() {
		return {
			VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
			VK_ACCESS_UNIFORM_READ_BIT,
			invalidImageLayout,
		};
	}
	static inline SyncScope topOfPipe(VkImageLayout layout) {
		return {
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, {}, layout,
		};
	}
	static inline SyncScope allAccess(VkPipelineStageFlags stage, VkImageLayout layout) {
		return {
			stage,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
			layout,
		};
	}
};

bool operator==(SyncScope a, SyncScope b);
bool operator!=(SyncScope a, SyncScope b);

bool writes(VkAccessFlags access);

// Only possible when both have same layout (or one has undefined layout).
SyncScope& operator|=(SyncScope& a, SyncScope b);
SyncScope operator|(SyncScope a, SyncScope b);

[[nodiscard]] inline VkImageSubresourceRange imageSubresourceRange(
		VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
		u32 baseLayer = 0u, u32 layerCount = VK_REMAINING_MIP_LEVELS,
		u32 baseLevel = 0u, u32 levelCount = VK_REMAINING_ARRAY_LAYERS) {
	VkImageSubresourceRange range;
	range.aspectMask = aspect;
	range.baseArrayLayer = baseLayer;
	range.layerCount = layerCount;
	range.baseMipLevel = baseLevel;
	range.levelCount = levelCount;
	return range;
}

[[nodiscard]] inline VkImageSubresourceRange fullSubresRange(
		VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
	return imageSubresourceRange(aspect);
}

struct ImageBarrier {
	VkImage image {};
	SyncScope src {};
	SyncScope dst {};
	VkImageSubresourceRange subres = fullSubresRange();
};

struct BufferBarrier {
	BufferSpan buffer;
	SyncScope src;
	SyncScope dst;
};

// Moves dst to src and fill dst with the given value
inline void nextDst(ImageBarrier& b, const SyncScope& newDst) {
	b.src = b.dst;
	b.dst = newDst;
}

void cmdBarrier(Device& dev, VkCommandBuffer cb,
	span<const ImageBarrier> imgBarriers,
	span<const BufferBarrier> bufBarriers);
void cmdBarrier(Device& dev, VkCommandBuffer cb, span<const ImageBarrier> barriers);
void cmdBarrier(Device& dev, VkCommandBuffer cb, VkImage image, SyncScope src,
	SyncScope dst, VkImageSubresourceRange subres = fullSubresRange());
void cmdBarrier(Device& dev, VkCommandBuffer cb, span<const BufferBarrier> barriers);
void cmdBarrier(Device& dev, VkCommandBuffer cb, BufferSpan, SyncScope src, SyncScope dst);

// Mainly for debugging
void cmdFullBarrier(Device& dev, VkCommandBuffer);

struct LocalBufferState {
	BufferSpan buf {};
	SyncScope lastAccess {};

	void transition(Device& dev, VkCommandBuffer cb, const SyncScope& newAccess) {
		if(lastAccess.access != VkAccessFlags{}) {
			cmdBarrier(dev, cb, buf, lastAccess, newAccess);
		}

		lastAccess = newAccess;
	}

	void discard(Device& dev, VkCommandBuffer cb, const SyncScope& newAccess) {
		BufferBarrier barrier{buf, SyncScope::discard(), newAccess};
		cmdBarrier(dev, cb, {{barrier}});
		lastAccess = newAccess;
	}
};

struct LocalImageState {
	VkImage image {};
	VkImageSubresourceRange range {imageSubresourceRange()};
	SyncScope lastAccess {};

	// avoid duplicate barriers
	struct {
		VkPipelineStageFlags stages {};
		VkAccessFlags access {};
	} sinceLastWrite_;

	void transition(Device& dev, VkCommandBuffer cb, const SyncScope& newAccess) {
		auto needsBarrier = true;
		if(writes(newAccess.access)) {
			sinceLastWrite_ = {};
		} else {
			needsBarrier =
				lastAccess.layout != newAccess.layout ||
				(sinceLastWrite_.stages & newAccess.stages) != 0u ||
				(sinceLastWrite_.access & newAccess.access) != 0u;
			sinceLastWrite_.stages |= newAccess.stages;
			sinceLastWrite_.access |= newAccess.access;
		}

		if(needsBarrier) {
			ImageBarrier barrier{image, lastAccess, newAccess, range};
			cmdBarrier(dev, cb, {{barrier}});
		}

		lastAccess = newAccess;
	}

	void discard(Device& dev, VkCommandBuffer cb, const SyncScope& newAccess) {
		ImageBarrier barrier{image, SyncScope::discard(), newAccess, range};
		cmdBarrier(dev, cb, {{barrier}});
		lastAccess = newAccess;
	}

	// void aliasDiscard(vk::CommandBuffer cb, const vpp::SyncScope& newAccess) {
	// 	vpp::ImageBarrier barrier{image, vpp::SyncScope::aliasDiscard(), newAccess, range};
	// 	vpp::cmdBarrier(cb, {{barrier}});
	// 	lastAccess = newAccess;
	// }
};

struct BarrierBatch {
	Device& dev;
	VkCommandBuffer cb;

	ThreadMemScope tms;
	ScopedVector<ImageBarrier> imageBarriers;
	ScopedVector<BufferBarrier> bufferBarriers;

	BarrierBatch(Device& xdev, VkCommandBuffer xcb) : dev(xdev), cb(xcb),
		imageBarriers(tms), bufferBarriers(tms) {}

	~BarrierBatch() {
		if(!imageBarriers.empty() || !bufferBarriers.empty()) {
			submit();
		}
	}

	BarrierBatch& transition(LocalImageState& state, const SyncScope& newAccess) {
		if(state.lastAccess.access != VkAccessFlags{}) {
			imageBarriers.push_back({state.image, state.lastAccess, newAccess, state.range});
		}

		state.lastAccess = newAccess;
		return *this;
	}

	BarrierBatch& transition(LocalBufferState& state, const SyncScope& newAccess) {
		if(state.lastAccess.access != VkAccessFlags{}) {
			bufferBarriers.push_back({state.buf, state.lastAccess, newAccess});
		}

		state.lastAccess = newAccess;
		return *this;
	}

	BarrierBatch& discard(LocalBufferState& state, const SyncScope& newAccess) {
		bufferBarriers.push_back({state.buf, SyncScope::discard(), newAccess});
		state.lastAccess = newAccess;
		return *this;
	}

	BarrierBatch& discard(LocalImageState& state, const SyncScope& newAccess) {
		imageBarriers.push_back({state.image, SyncScope::discard(), newAccess, state.range});
		state.lastAccess = newAccess;
		return *this;
	}

	void submit() {
		cmdBarrier(dev, cb, imageBarriers, bufferBarriers);
		imageBarriers.clear();
		bufferBarriers.clear();
	}
};

} // namespace vil::vku
