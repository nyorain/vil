// This code is compiled into vil itself. But run in an integration
// test context, i.e. inside our application that correctly
// initializes everything.

#include <wrap.hpp>
#include <gui/gui.hpp>
#include <command/commands.hpp>
#include <command/desc.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/state.hpp>
#include <threadContext.hpp>
#include <layer.hpp>
#include <image.hpp>
#include <queue.hpp>
#include <cb.hpp>
#include <rp.hpp>
#include "./internal.hpp"
#include "../data/simple.comp.spv.h" // see simple.comp; compiled manually

using namespace tut;

namespace vil::test {

VkCommandPool setupCommandPool() {
	auto& stp = gSetup;
	VkCommandPoolCreateInfo cpi {};
	cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = stp.qfam;
	VkCommandPool cmdPool;
	VK_CHECK(CreateCommandPool(stp.dev, &cpi, nullptr, &cmdPool));
	return cmdPool;
}

VkCommandBuffer allocCommandBuffer(VkCommandPool cmdPool,
		VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
	auto& stp = gSetup;
	VkCommandBufferAllocateInfo cbai {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandBufferCount = 1u;
	cbai.commandPool = cmdPool;
	cbai.level = level;
	VkCommandBuffer cb;
	VK_CHECK(AllocateCommandBuffers(stp.dev, &cbai, &cb));
	return cb;
}

TEST(int_basic) {
	auto& stp = gSetup;

	// setup texture
	auto tc = TextureCreation();
	auto tex = Texture(stp, tc);

	// setup render pass
	auto passes = {0u};
	auto format = tc.ici.format;
	auto rpi = renderPassInfo({{format}}, {{passes}});

	// add dummy VK_ATTACHMENT_UNUSED depth stencil attachment
	VkAttachmentReference unusedRef;
	unusedRef.attachment = VK_ATTACHMENT_UNUSED;
	unusedRef.layout = VK_IMAGE_LAYOUT_UNDEFINED;
	rpi.subpasses[0].pDepthStencilAttachment = &unusedRef;

	VkRenderPass rp;
	VK_CHECK(CreateRenderPass(stp.dev, &rpi.info(), nullptr, &rp));

	// setup fb
	VkFramebuffer fb;
	VkFramebufferCreateInfo fbi = {};
	fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fbi.attachmentCount = 1;
	fbi.pAttachments = &tex.imageView;
	fbi.renderPass = rp;
	fbi.width = tc.ici.extent.width;
	fbi.height = tc.ici.extent.height;
	fbi.layers = 1;
	VK_CHECK(CreateFramebuffer(stp.dev, &fbi, nullptr, &fb));

	// setup command pool & buffer
	VkCommandPool cmdPool = setupCommandPool();
	VkCommandBuffer cb = allocCommandBuffer(cmdPool);

	auto& vilCB = unwrap(cb);
	dlg_assert(vilCB.state() == CommandBuffer::State::initial);

	// record commands
	VkCommandBufferBeginInfo cbi {};
	cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(BeginCommandBuffer(cb, &cbi));

	// incorrect label hierarchy test
	VkDebugUtilsLabelEXT label {};
	label.pLabelName = "TestLabel1";
	label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;

	VkClearValue clearValue {};
	VkRenderPassBeginInfo rbi {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = rp;
	rbi.renderArea.extent.width = tc.ici.extent.width;
	rbi.renderArea.extent.height = tc.ici.extent.height;
	rbi.clearValueCount = 1u;
	rbi.pClearValues = &clearValue;
	rbi.framebuffer = fb;

	CmdBeginDebugUtilsLabelEXT(cb, &label);
	CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	CmdEndDebugUtilsLabelEXT(cb);
	CmdEndRenderPass(cb);

	// just pop labels that were never pushed - valid behavior per spec
	CmdEndDebugUtilsLabelEXT(cb);
	CmdEndDebugUtilsLabelEXT(cb);

	// other case of hierarchy mismatch
	// we just don't end it inside the scope
	label.pLabelName = "TestLabel2";
	CmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
	CmdBeginDebugUtilsLabelEXT(cb, &label);
	CmdEndRenderPass(cb);

	// have some unterminated labels
	label.pLabelName = "Unterminated1";
	CmdBeginDebugUtilsLabelEXT(cb, &label);
	label.pLabelName = "Unterminated2";
	CmdBeginDebugUtilsLabelEXT(cb, &label);

	EndCommandBuffer(cb);
	dlg_assert(vilCB.state() == CommandBuffer::State::executable);

	// submit it, make sure it's hooked
	auto& rec = *vilCB.lastRecordPtr();
	auto* cmd = rec.commands->children_;
	auto dst = cmd->next->next;

	auto& vilDev = *stp.vilDev;
	CommandHook::HookOps ops {};
	ops.queryTime = true;
	vilDev.commandHook->ops(std::move(ops));

	CommandHook::HookTarget target {};
	target.all = true;
	vilDev.commandHook->target(std::move(target));

	vilDev.commandHook->forceHook.store(true);
	vilDev.commandHook->desc(vilCB.lastRecordPtr(), {dst}, {});

	VkSubmitInfo si {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb;
	QueueSubmit(stp.queue, 1u, &si, VK_NULL_HANDLE);

	DeviceWaitIdle(stp.dev);

	dlg_assert(vilDev.commandHook->moveCompleted().size() == 1u);

	// cleanup
	DestroyFramebuffer(stp.dev, fb, nullptr);
	DestroyRenderPass(stp.dev, rp, nullptr);
	DestroyCommandPool(stp.dev, cmdPool, nullptr);
}

TEST(int_gui) {
	auto& stp = gSetup;
	auto& vilDev = *stp.vilDev;

	// init gui
	auto gui = std::make_unique<vil::Gui>();
	gui->init(vilDev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, true);
	// TODO: actually render gui stuff. Create own swapchain?
	//   or rather implement render-on-image for that?
	//   We could create a headless surface tho.
}

TEST(int_secondary_cb) {
	auto& setup = gSetup;

	// matching of secondary command buffers
	// setup pool
	VkCommandPool cmdPool = setupCommandPool();
	VkCommandBuffer scb = allocCommandBuffer(cmdPool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);

	{
		VkCommandBufferInheritanceInfo ii {};
		ii.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;

		VkCommandBufferBeginInfo beginInfo {};
		beginInfo.pInheritanceInfo = &ii;
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
		VK_CHECK(BeginCommandBuffer(scb, &beginInfo));

		// some dummy label
		VkDebugUtilsLabelEXT label {};
		label.pLabelName = "TestLabel1";
		label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;

		CmdBeginDebugUtilsLabelEXT(scb, &label);
		CmdEndDebugUtilsLabelEXT(scb);

		EndCommandBuffer(scb);
	}

	// setup primary cbs
	VkCommandBuffer pcbs[2];
	pcbs[0] = allocCommandBuffer(cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
	pcbs[1] = allocCommandBuffer(cmdPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);

	for(auto& cb : pcbs) {
		VkCommandBufferBeginInfo beginInfo {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		VK_CHECK(BeginCommandBuffer(cb, &beginInfo));

		CmdExecuteCommands(cb, 1u, &scb);
		CmdExecuteCommands(cb, 1u, &scb);

		EndCommandBuffer(cb);
	}

	auto& cb0 = unwrap(pcbs[0]);
	auto& cb1 = unwrap(pcbs[1]);

	auto& rec0 = *cb0.lastRecordLocked();
	auto& rec1 = *cb1.lastRecordLocked();

	dlg_assert(rec0.commands->sectionStats().numChildSections == 2u);

	auto& execCmd0 = dynamic_cast<ExecuteCommandsCmd&>(*rec0.commands->firstChildParent());
	dlg_assert(execCmd0.sectionStats().numChildSections == 1u);
	dlg_assert(execCmd0.nextParent_);

	auto& childCmd0 = dynamic_cast<ExecuteCommandsChildCmd&>(*execCmd0.firstChildParent());
	dlg_assert(childCmd0.sectionStats().numChildSections == 1u);
	dlg_assert(childCmd0.firstChildParent());

	auto& execCmd1 = dynamic_cast<ExecuteCommandsCmd&>(*rec1.commands->firstChildParent());
	dlg_assert(execCmd1.sectionStats().numChildSections == 1u);
	dlg_assert(execCmd1.nextParent_);

	auto& childCmd1 = dynamic_cast<ExecuteCommandsChildCmd&>(*execCmd1.firstChildParent());
	dlg_assert(childCmd1.sectionStats().numChildSections == 1u);
	dlg_assert(childCmd1.firstChildParent());

	ThreadMemScope tms;
	auto res = match(tms, *rec0.commands, *rec1.commands);
	auto matcher = res.second;
	dlg_assert(matcher.match == matcher.total);
	dlg_assert(matcher.match > 0.f);

	auto secm = res.first;
	dlg_assert(secm.size() == 2u);
	dlg_assert(secm[1].a == &execCmd0 && secm[1].b == &execCmd1);
	dlg_assert(secm[1].children.size() == 1u);
	dlg_assert(secm[0].a == execCmd0.nextParent_ && secm[0].b == execCmd1.nextParent_);
	dlg_assert(secm[0].children.size() == 1u);

	{
		auto secm = res.first[1].children;
		dlg_assert(secm[0].a == &childCmd0 && secm[0].b == &childCmd1);
		dlg_assert(secm[0].children.size() == 1u); // for the label section
	}

	DestroyCommandPool(setup.dev, cmdPool, nullptr);
}

TEST(int_copy_transfer) {
	auto& stp = gSetup;

	// setup texture
	auto tc = TextureCreation();
	auto tex0 = Texture(stp, tc);
	auto tex1 = Texture(stp, tc);

	// setup command pool & buffer
	VkCommandPool cmdPool = setupCommandPool();
	VkCommandBuffer cb = allocCommandBuffer(cmdPool);

	auto& vilCB = unwrap(cb);
	dlg_assert(vilCB.state() == CommandBuffer::State::initial);

	// record commands
	VkCommandBufferBeginInfo cbi {};
	cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(BeginCommandBuffer(cb, &cbi));

	// barrier
	VkImageMemoryBarrier imgBarriers[2] {};

	imgBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgBarriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
	imgBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imgBarriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	imgBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imgBarriers[0].subresourceRange.layerCount = 1u;
	imgBarriers[0].subresourceRange.levelCount = 1u;
	imgBarriers[0].image = tex0.image;

	imgBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgBarriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	imgBarriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imgBarriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	imgBarriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imgBarriers[1].subresourceRange.layerCount = 1u;
	imgBarriers[1].subresourceRange.levelCount = 1u;
	imgBarriers[1].image = tex1.image;

	CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0u, nullptr, 0u, nullptr,
		2u, imgBarriers);

	// copy images
	VkImageCopy region {};
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.layerCount = 1u;
	region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.dstSubresource.layerCount = 1u;
	region.extent = tc.ici.extent;
	CmdCopyImage(cb, tex0.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		tex1.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &region);

	EndCommandBuffer(cb);
	dlg_assert(vilCB.state() == CommandBuffer::State::executable);

	// submit it, make sure it's hooked
	auto& rec = *vilCB.lastRecordPtr();
	auto* cmd = rec.commands->children_;
	auto dst = cmd->next;
	dlg_assert(dynamic_cast<CopyImageCmd*>(dst));

	auto& vilDev = *stp.vilDev;
	CommandHook::HookOps ops {};
	ops.copyTransferSrc = true;
	vilDev.commandHook->ops(std::move(ops));

	CommandHook::HookTarget target {};
	target.all = true;
	vilDev.commandHook->target(std::move(target));

	vilDev.commandHook->forceHook.store(true);
	vilDev.commandHook->desc(vilCB.lastRecordPtr(), {dst}, {});

	VkSubmitInfo si {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb;
	QueueSubmit(stp.queue, 1u, &si, VK_NULL_HANDLE);

	DeviceWaitIdle(stp.dev);

	auto completed = vilDev.commandHook->moveCompleted();
	dlg_assert(completed.size() == 1u);
	dlg_assert(completed[0].command[0] == dst);
	dlg_assert(completed[0].state->transferImgCopy.image);

	// cleanup
	DestroyCommandPool(stp.dev, cmdPool, nullptr);
}

struct PipeSetup {
	VkDescriptorSetLayout dsLayout {};
	VkDescriptorPool dsPool {};
	VkDescriptorSet ds {};
	VkPipelineLayout pipeLayout {};
	VkPipeline pipe {};
};

void init(PipeSetup& ps) {
	auto& stp = gSetup;

	auto poolSizes = std::array {
		VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
		VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
	};

	VkDescriptorPoolCreateInfo dci {};
	dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dci.pPoolSizes = poolSizes.data();
	dci.poolSizeCount = poolSizes.size();
	dci.maxSets = 1u;
	VK_CHECK(CreateDescriptorPool(stp.dev, &dci, nullptr, &ps.dsPool));

	// layout
	auto bindings = std::array {
		VkDescriptorSetLayoutBinding {0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			1u, VK_SHADER_STAGE_ALL, nullptr},
		VkDescriptorSetLayoutBinding {1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			1u, VK_SHADER_STAGE_ALL, nullptr},
	};

	VkDescriptorSetLayoutCreateInfo lci {};
	lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	lci.bindingCount = 1u;
	lci.pBindings = bindings.data();
	lci.bindingCount = bindings.size();
	VK_CHECK(CreateDescriptorSetLayout(stp.dev, &lci,
		nullptr, &ps.dsLayout));

	// pipeline layout
	VkPipelineLayoutCreateInfo plci {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1u;
	plci.pSetLayouts = &ps.dsLayout;
	VK_CHECK(CreatePipelineLayout(stp.dev, &plci, nullptr, &ps.pipeLayout));

	// shader mod
	VkShaderModuleCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	sci.codeSize = sizeof(simple_comp_spv_data);
	sci.pCode = simple_comp_spv_data;
	VkShaderModule mod;
	VK_CHECK(CreateShaderModule(stp.dev, &sci, nullptr, &mod));

	// pipe
	VkComputePipelineCreateInfo cpi {};
	cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpi.layout = ps.pipeLayout;
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.module = mod;
	cpi.stage.pName = "main";
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	VK_CHECK(CreateComputePipelines(stp.dev, {}, 1u, &cpi, nullptr, &ps.pipe));

	DestroyShaderModule(stp.dev, mod, nullptr);

	// alloc ds
	VkDescriptorSetAllocateInfo dsai {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = ps.dsPool;
	dsai.pSetLayouts = &ps.dsLayout;
	dsai.descriptorSetCount = 1u;
	VK_CHECK(AllocateDescriptorSets(stp.dev, &dsai, &ps.ds));
}

void destroy(PipeSetup& ps) {
	auto& stp = gSetup;

	// no need to free ds
	DestroyPipeline(stp.dev, ps.pipe, nullptr);
	DestroyPipelineLayout(stp.dev, ps.pipeLayout, nullptr);
	DestroyDescriptorPool(stp.dev, ps.dsPool, nullptr);
	DestroyDescriptorSetLayout(stp.dev, ps.dsLayout, nullptr);
}

void sampleCopyTest(VkCommandBuffer cb, PipeSetup& ps, Texture& tex0) {
	auto& stp = gSetup;

	auto& vilCB = unwrap(cb);

	// record commands
	VkCommandBufferBeginInfo cbi {};
	cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	VK_CHECK(BeginCommandBuffer(cb, &cbi));

	// barrier
	VkImageMemoryBarrier imgBarriers[1] {};
	imgBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgBarriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
	imgBarriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imgBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imgBarriers[0].subresourceRange.layerCount = 1u;
	imgBarriers[0].subresourceRange.levelCount = 1u;
	imgBarriers[0].image = tex0.image;

	CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0u, nullptr, 0u, nullptr,
		1u, imgBarriers);

	CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ps.pipe);
	CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, ps.pipeLayout,
		0u, 1u, &ps.ds, 0u, nullptr);
	CmdDispatch(cb, 1u, 1u, 1u);

	EndCommandBuffer(cb);
	dlg_assert(vilCB.state() == CommandBuffer::State::executable);

	// submit it, make sure it's hooked
	auto& rec = *vilCB.lastRecordPtr();
	auto* cmd = rec.commands->children_;
	auto dst = cmd->next->next->next;
	dlg_assert(dynamic_cast<DispatchCmd*>(dst));

	auto& vilDev = *stp.vilDev;
	CommandHook::HookOps ops {};
	auto& dsCopy = ops.descriptorCopies.emplace_back();
	dsCopy.before = true;
	dsCopy.binding = 0u;
	dsCopy.set = 0u;
	dsCopy.imageAsBuffer = true;
	vilDev.commandHook->ops(std::move(ops));

	CommandHook::HookTarget target {};
	target.all = true;
	vilDev.commandHook->target(std::move(target));

	vilDev.commandHook->forceHook.store(true);
	vilDev.commandHook->desc(vilCB.lastRecordPtr(), {dst}, {});

	VkSubmitInfo si {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1u;
	si.pCommandBuffers = &cb;
	QueueSubmit(stp.queue, 1u, &si, VK_NULL_HANDLE);

	DeviceWaitIdle(stp.dev);

	auto completed = vilDev.commandHook->moveCompleted();
	dlg_assert(completed.size() == 1u);
	dlg_assert(completed[0].command[0] == dst);
	auto& state = *completed[0].state;
	dlg_assert(state.copiedDescriptors.size() == 1u);
	auto& imgAsBuf = std::get<CopiedImageToBuffer>(state.copiedDescriptors[0].data);
	dlg_assert(imgAsBuf.buffer.buf);
}

TEST(int_sample_copy_pipeline) {
	auto& stp = gSetup;

	// setup command pool & buffer
	VkCommandPool cmdPool = setupCommandPool();
	VkCommandBuffer cb = allocCommandBuffer(cmdPool);

	// setup pipe
	PipeSetup ps;
	init(ps);

	auto buf0 = tut::Buffer(stp, 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
	auto sci = linearSamplerCI();
	VkSampler sampler;
	VK_CHECK(CreateSampler(stp.dev, &sci, nullptr, &sampler));

	auto formats = {
		VK_FORMAT_R16G16B16A16_SFLOAT,
		VK_FORMAT_B10G11R11_UFLOAT_PACK32,
		VK_FORMAT_BC1_RGB_UNORM_BLOCK,
		// TODO: test depth
	};

	for(auto format : formats) {
		auto tc = TextureCreation(format);
		auto tex0 = Texture(stp, tc);

		VkDescriptorImageInfo imgInfo {};
		imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		imgInfo.imageView = tex0.imageView;
		imgInfo.sampler = sampler;

		VkDescriptorBufferInfo bufInfo {};
		bufInfo.range = VK_WHOLE_SIZE;
		bufInfo.buffer = buf0.buffer;

		VkWriteDescriptorSet dsw[2] {};
		dsw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		dsw[0].descriptorCount = 1u;
		dsw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		dsw[0].pImageInfo = &imgInfo;
		dsw[0].dstSet = ps.ds;
		dsw[0].dstBinding = 0u;

		dsw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		dsw[1].descriptorCount = 1u;
		dsw[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		dsw[1].pBufferInfo = &bufInfo;
		dsw[1].dstSet = ps.ds;
		dsw[1].dstBinding = 1u;

		UpdateDescriptorSets(stp.dev, 2u, dsw, 0u, nullptr);

		sampleCopyTest(cb, ps, tex0);
	}

	// cleanup
	destroy(ps);
	DestroySampler(stp.dev, sampler, nullptr);
	DestroyCommandPool(stp.dev, cmdPool, nullptr);
}

// TODO: write test where we record a command buffer that executes
// each command once. Then hook each of those commands, separately.

} // namespace vil::test
