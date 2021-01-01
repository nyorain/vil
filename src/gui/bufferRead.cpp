#include <gui/bufferRead.hpp>
#include <util.hpp>
#include <buffer.hpp>
#include <commands.hpp>
#include <cb.hpp>
#include <record.hpp>

namespace fuen {

struct IndirectCommandHookRecord : CommandHookRecordImpl {
	IndirectCommandHook* hook {};
	VkDeviceMemory dstMemory {};
	VkBuffer dstBuffer {};
	void* map {};
	Command* hooked {}; // TODO(opt) could be stored in CommandHookRecord

	IndirectCommandHookRecord(IndirectCommandHook&, CommandBuffer&, Command&);
	void recordBeforeHooked(Device&, VkCommandBuffer, Command&) override;
	void finish(Device& dev) noexcept override;
	std::unique_ptr<CommandHookSubmissionImpl> submit() override;
	void invalidate() override { hook = nullptr; }
	SimultaneousSubmitHook simulataneousBehavior() const override {
		return SimultaneousSubmitHook::skip;
	}
};

struct IndirectCommandHookSubmission : CommandHookSubmissionImpl {
	IndirectCommandHookRecord* record {};

	IndirectCommandHookSubmission(IndirectCommandHookRecord& r) : record(&r) {}
	void finish(Device& dev) noexcept override;
};

std::unique_ptr<CommandHookRecordImpl> IndirectCommandHook::createRecord(
		CommandBuffer& cb, Command& hooked) {
	return std::make_unique<IndirectCommandHookRecord>(*this, cb, hooked);
}

IndirectCommandHookRecord::IndirectCommandHookRecord(
		IndirectCommandHook& xhook, CommandBuffer& cb, Command& hooked) {
	this->hook = &xhook;
	this->hooked = &hooked;

	VkDeviceSize dstSize {};
	switch(hook->type) {
		case IndirectCommandHook::Type::dispatch: {
			// auto& cmd = dynamic_cast<DispatchIndirectCmd&>(hooked);
			dstSize = sizeof(VkDispatchIndirectCommand);
			break;
		} case IndirectCommandHook::Type::draw: {
			auto& cmd = dynamic_cast<DrawIndirectCmd&>(hooked);
			VkDeviceSize stride = cmd.indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd.stride ? cmd.stride : stride;
			dstSize = cmd.drawCount * stride;
			break;
		} case IndirectCommandHook::Type::drawCount: {
			auto& cmd = dynamic_cast<DrawIndirectCountCmd&>(hooked);
			VkDeviceSize stride = cmd.indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd.stride ? cmd.stride : stride;
			// we get the maximum copy size via maxDrawCount but also
			// via the remaining buffer size.
			auto remSize = cmd.buffer->ci.size - cmd.offset;
			dstSize = std::min(cmd.maxDrawCount * stride, remSize);
			dstSize += sizeof(u32); // for the count
			break;
		}
	}

	auto& dev = *cb.dev;
	VkBufferCreateInfo bci {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bci.size = dstSize;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VK_CHECK(dev.dispatch.CreateBuffer(dev.handle, &bci, nullptr, &dstBuffer));
	nameHandle(dev, this->dstBuffer, "IndirectCommandHookRecord:dstBuffer");

	VkMemoryRequirements memReqs;
	dev.dispatch.GetBufferMemoryRequirements(dev.handle, dstBuffer, &memReqs);

	// new memory
	VkMemoryAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = findLSB(memReqs.memoryTypeBits & dev.hostVisibleMemTypeBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &dstMemory));
	nameHandle(dev, this->dstMemory, "IndirectCommandHookRecord:dstMemory");

	VK_CHECK(dev.dispatch.BindBufferMemory(dev.handle, dstBuffer, dstMemory, 0));

	// map memory
	VK_CHECK(dev.dispatch.MapMemory(dev.handle, dstMemory, 0, VK_WHOLE_SIZE, 0, &map));
}

void IndirectCommandHookRecord::recordBeforeHooked(Device& dev,
		VkCommandBuffer cb, Command& hooked) {

	// TODO: technically, we probably need a barrier for out dstBuffer
	// after the copie(s) as well.
	switch(hook->type) {
		case IndirectCommandHook::Type::dispatch: {
			auto& cmd = dynamic_cast<DispatchIndirectCmd&>(hooked);

			VkBufferCopy copy;
			copy.srcOffset = cmd.offset;
			copy.dstOffset = 0u;
			copy.size = sizeof(VkDispatchIndirectCommand);

			VkBufferMemoryBarrier barrier {};
			barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			barrier.buffer = cmd.buffer->handle;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
			barrier.size = copy.size;
			barrier.offset = copy.srcOffset;

			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dunno
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 1, &barrier, 0, nullptr);

			dev.dispatch.CmdCopyBuffer(cb, cmd.buffer->handle, dstBuffer, 1, &copy);

			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // dunno
				0, 0, nullptr, 1, &barrier, 0, nullptr);

			break;
		} case IndirectCommandHook::Type::draw: {
			auto& cmd = dynamic_cast<DrawIndirectCmd&>(hooked);

			VkDeviceSize stride = cmd.indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			stride = cmd.stride ? cmd.stride : stride;
			auto dstSize = cmd.drawCount * stride;

			VkBufferCopy copy;
			copy.srcOffset = cmd.offset;
			copy.dstOffset = 0u;
			copy.size = dstSize;

			VkBufferMemoryBarrier barrier {};
			barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
			barrier.buffer = cmd.buffer->handle;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
			barrier.size = copy.size;
			barrier.offset = copy.srcOffset;

			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dunno
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 1, &barrier, 0, nullptr);

			dev.dispatch.CmdCopyBuffer(cb, cmd.buffer->handle, dstBuffer, 1, &copy);

			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; // dunno
			dev.dispatch.CmdPipelineBarrier(cb,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, // dunno
				0, 0, nullptr, 1, &barrier, 0, nullptr);

			break;
		} case IndirectCommandHook::Type::drawCount: {
			// auto& cmd = dynamic_cast<DrawIndirectCountCmd&>(hooked);
			dlg_error("Not implemented");
			break;
		}
	}
}

std::unique_ptr<CommandHookSubmissionImpl> IndirectCommandHookRecord::submit() {
	return std::make_unique<IndirectCommandHookSubmission>(*this);
}

void IndirectCommandHookRecord::finish(Device& dev) noexcept {
	dev.dispatch.FreeMemory(dev.handle, dstMemory, nullptr);
	dev.dispatch.DestroyBuffer(dev.handle, dstBuffer, nullptr);
}

//
void IndirectCommandHookSubmission::finish(Device& dev) noexcept {
	if(!record->hook) {
		return;
	}

	// TODO: only call on non-coherent memory
	VkMappedMemoryRange range {};
	range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
	range.offset = 0;
	range.size = VK_WHOLE_SIZE;
	range.memory = record->dstMemory;
	VK_CHECK(dev.dispatch.InvalidateMappedMemoryRanges(dev.handle, 1, &range));

	switch(record->hook->type) {
		case IndirectCommandHook::Type::dispatch: {
			record->hook->count = 1u;
			auto size = sizeof(VkDispatchIndirectCommand);
			record->hook->data.resize(size);
			std::memcpy(record->hook->data.data(), record->map, size);
			break;
		} case IndirectCommandHook::Type::draw: {
			auto& cmd = dynamic_cast<DrawIndirectCmd&>(*record->hooked);
			VkDeviceSize cmdSize = cmd.indexed ?
				sizeof(VkDrawIndexedIndirectCommand) :
				sizeof(VkDrawIndirectCommand);
			auto stride = cmd.stride ? cmd.stride : cmdSize;

			record->hook->count = cmd.drawCount;
			record->hook->data.resize(cmd.drawCount * cmdSize);

			for(auto i = 0u; i < cmd.drawCount; ++i) {
				auto src = reinterpret_cast<std::byte*>(record->map) + i * stride;
				auto dst = record->hook->data.data() + i * cmdSize;
				std::memcpy(dst, src, cmdSize);
			}

			break;
		} case IndirectCommandHook::Type::drawCount: {
			dlg_error("Not implemented");
			break;
		}
	}
}


} // namespace fuen
