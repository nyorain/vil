#include <gui/blur.hpp>
#include <device.hpp>
#include <threadContext.hpp>
#include <blur.comp.spv.h>

namespace vil {

GuiBlur::~GuiBlur() {
	destroy(*this);
}

void init(GuiBlur& blur, Device& dev) {
	blur.dev = &dev;

	// Create sampler
	VkSamplerCreateInfo saci {};
	saci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	saci.magFilter = VK_FILTER_LINEAR;
	saci.minFilter = VK_FILTER_LINEAR;
	saci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	saci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	saci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	saci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	saci.minLod = -1000;
	saci.maxLod = 1000;
	saci.maxAnisotropy = 1.0f;
	VK_CHECK(dev.dispatch.CreateSampler(dev.handle, &saci, nullptr, &blur.sampler));
	nameHandle(dev, blur.sampler, "BlurSampler");

	// Create ds layout
	VkDescriptorSetLayoutBinding bindings[2] {};
	bindings[0].binding = 0u;
	bindings[0].descriptorCount = 1u;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[0].pImmutableSamplers = &blur.sampler;

	bindings[1].binding = 1u;
	bindings[1].descriptorCount = 1u;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo dslci {};
	dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = 2u;
	dslci.pBindings = bindings;
	VK_CHECK(dev.dispatch.CreateDescriptorSetLayout(dev.handle, &dslci, nullptr, &blur.dsLayout));
	nameHandle(dev, blur.dsLayout, "GuiBlur:dsLayout");

	// create pipe layout
	VkPushConstantRange pcrs[1] = {};
	pcrs[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcrs[0].offset = 0;
	pcrs[0].size = 8;

	VkPipelineLayoutCreateInfo plci {};
	plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &blur.dsLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = pcrs;
	VK_CHECK(dev.dispatch.CreatePipelineLayout(dev.handle, &plci, nullptr, &blur.pipeLayout));
	nameHandle(dev, blur.pipeLayout, "GuiBlur:pipeLayout");

	// load shader module
	VkShaderModule mod {};

	VkShaderModuleCreateInfo sci {};
	sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	sci.codeSize = sizeof(blur_comp_spv_data);
	sci.pCode = blur_comp_spv_data;

	VK_CHECK(dev.dispatch.CreateShaderModule(dev.handle, &sci, nullptr, &mod));

	// create pipeline
	VkComputePipelineCreateInfo cpi {};
	cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpi.layout = blur.pipeLayout;
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = mod;
	cpi.stage.pName = "main";

	VK_CHECK(dev.dispatch.CreateComputePipelines(dev.handle, VK_NULL_HANDLE,
		1u, &cpi, nullptr, &blur.blurPipe));
	nameHandle(dev, blur.blurPipe, "GuiBlur:pipe");

	dev.dispatch.DestroyShaderModule(dev.handle, mod, nullptr);

	// init vertices
	blur.vertices.ensure(dev, sizeof(ImDrawVert) * 6, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	auto* verts = reinterpret_cast<ImDrawVert*>(blur.vertices.map);

	auto col = ImColor(1.f, 1.f, 1.f, 1.f);
	auto writeVert = [&](unsigned i, float x, float y) {
		verts[i].col = col;
		verts[i].uv.x = 0.5f + 0.5f * x;
		verts[i].uv.y = 0.5f + 0.5f * y;
		verts[i].pos.x = x;
		verts[i].pos.y = y;
	};

	// triangle 1
	writeVert(0, -1.f, -1.f);
	writeVert(1, 1.f, -1.f);
	writeVert(2, 1.f, 1.f);

	// triangle 2
	writeVert(3, -1.f, -1.f);
	writeVert(4, 1.f, 1.f);
	writeVert(5, -1.f, 1.f);

	blur.vertices.flushMap();
}

void destroyBuffers(GuiBlur& blur, VkDescriptorPool dsPool) {
	if(blur.renderBuffers.empty()) {
		return;
	}

	auto& dev = *blur.dev;
	dev.dispatch.FreeDescriptorSets(dev.handle, dsPool, 2u, blur.steps.data());
	blur.steps = {};

	ThreadMemScope memScope;
	auto sets = memScope.alloc<VkDescriptorSet>(blur.renderBuffers.size());

	for(auto i = 0u; i < sets.size(); ++i) {
		auto& buf = blur.renderBuffers[i];
		dev.dispatch.DestroyImageView(dev.handle, buf.view, nullptr);
		sets[i] = buf.input;
		buf = {};
	}

	dev.dispatch.FreeDescriptorSets(dev.handle, dsPool, u32(sets.size()), sets.data());

	dev.dispatch.DestroyImageView(dev.handle, blur.view0, nullptr);
	dev.dispatch.DestroyImageView(dev.handle, blur.view1, nullptr);
	dev.dispatch.DestroyImage(dev.handle, blur.target, nullptr);
	dev.dispatch.FreeMemory(dev.handle, blur.targetMemory, nullptr);

	blur.view0 = {};
	blur.view1 = {};
	blur.target = {};
	blur.targetMemory = {};
	blur.renderBuffers.resize(0);
}

void resize(GuiBlur& blur, VkExtent2D size, VkSwapchainKHR swapchain,
		VkFormat swapchainFormat) {
	auto& dev = *blur.dev;
	auto dsPool = dev.dsPool;
	destroyBuffers(blur, dsPool);

	// create target image
	VkImageCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.arrayLayers = 2u;
	ici.extent = {size.width / 2, size.height / 2, 1u};
	ici.format = blur.targetFormat;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.mipLevels = 1u;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;

	VK_CHECK(dev.dispatch.CreateImage(dev.handle, &ici, nullptr, &blur.target));
	nameHandle(dev, blur.target, "GuiBlur:target");

	// bind memory for the image
	VkMemoryRequirements memReqs;
	dev.dispatch.GetImageMemoryRequirements(dev.handle, blur.target, &memReqs);

	auto memBits = memReqs.memoryTypeBits & dev.deviceLocalMemTypeBits;
	VkMemoryAllocateInfo allocInfo {};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = findLSB(memBits);
	VK_CHECK(dev.dispatch.AllocateMemory(dev.handle, &allocInfo, nullptr, &blur.targetMemory));
	nameHandle(dev, blur.targetMemory, "GuiBlur:targetMemory");

	VK_CHECK(dev.dispatch.BindImageMemory(dev.handle, blur.target, blur.targetMemory, 0));

	// create target views
	VkImageViewCreateInfo ivi {};
	ivi.image = blur.target;
	ivi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivi.format = blur.targetFormat;
	ivi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	ivi.subresourceRange.baseArrayLayer = 0u;
	ivi.subresourceRange.baseMipLevel = 0u;
	ivi.subresourceRange.layerCount = 1u;
	ivi.subresourceRange.levelCount = 1u;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &blur.view0));

	ivi.subresourceRange.baseArrayLayer = 1u;
	VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &blur.view1));

	// create image views & descriptor sets
	u32 imgCount;
	VK_CHECK(dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapchain, &imgCount, nullptr));

	ThreadMemScope memScope;
	auto imgs = memScope.alloc<VkImage>(imgCount);

	VK_CHECK(dev.dispatch.GetSwapchainImagesKHR(dev.handle, swapchain, &imgCount, imgs.data()));

	auto sets = memScope.alloc<VkDescriptorSet>(imgCount + 2);
	auto layouts = memScope.alloc<VkDescriptorSetLayout>(imgCount + 2);
	for(auto& layout : layouts) {
		layout = blur.dsLayout;
	}

	VkDescriptorSetAllocateInfo dai {};
	dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dai.descriptorSetCount = sets.size();
	dai.pSetLayouts = layouts.data();
	dai.descriptorPool = dsPool;
	VK_CHECK(dev.dispatch.AllocateDescriptorSets(dev.handle, &dai, sets.data()));

	ivi.format = swapchainFormat;
	ivi.subresourceRange.baseArrayLayer = 0u;

	auto inputInfos = memScope.alloc<VkDescriptorImageInfo>(imgCount + 2);
	auto outputInfos = memScope.alloc<VkDescriptorImageInfo>(imgCount + 2);
	auto writes = memScope.alloc<VkWriteDescriptorSet>(2 * (imgCount + 2));

	auto addDSWrite = [&](u32 i, VkImageView input, VkImageView output) {
		auto& inputWrite = writes[2 * i];
		auto& outputWrite = writes[2 * i + 1];

		inputInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		inputInfos[i].imageView = input;
		inputInfos[i].sampler = {};

		inputWrite.pImageInfo = &inputInfos[i];
		inputWrite.descriptorCount = 1u;
		inputWrite.dstArrayElement = 0u;
		inputWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		inputWrite.dstSet = sets[i];
		inputWrite.dstBinding = 0u;
		inputWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

		outputInfos[i].imageView = output;
		outputInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		outputInfos[i].sampler = {};

		outputWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		outputWrite.pImageInfo = &outputInfos[i];
		outputWrite.descriptorCount = 1u;
		outputWrite.dstArrayElement = 0u;
		outputWrite.dstSet = sets[i];
		outputWrite.dstBinding = 1u;
		outputWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	};

	blur.renderBuffers.resize(imgCount);

	for(auto i = 0u; i < imgCount; ++i) {
		auto& buf = blur.renderBuffers[i];
		buf.input = sets[i];
		buf.image = imgs[i];

		ivi.image = imgs[i];
		VK_CHECK(dev.dispatch.CreateImageView(dev.handle, &ivi, nullptr, &buf.view));

		addDSWrite(i, buf.view, blur.view0);
	}

	blur.steps[0] = sets[imgCount + 0];
	blur.steps[1] = sets[imgCount + 1];

	addDSWrite(imgCount + 0, blur.view0, blur.view1);
	addDSWrite(imgCount + 1, blur.view1, blur.view0);

	dev.dispatch.UpdateDescriptorSets(dev.handle, u32(writes.size()),
		writes.data(), 0u, nullptr);

	blur.size = size;
}

void blur(GuiBlur& blur, VkCommandBuffer cb, u32 imageIdx, VkOffset2D offset, VkExtent2D size) {
	dlg_assert(imageIdx < blur.renderBuffers.size());
	auto& dev = *blur.dev;

	(void) offset;
	(void) size;

	auto& rbuf = blur.renderBuffers[imageIdx];

	// make sure we can read from swapchain image - bring it into readOnlyOptimal state.
	VkImageMemoryBarrier imgb[2] = {};
	imgb[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgb[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	imgb[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	imgb[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	imgb[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgb[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgb[0].image = rbuf.image;
	imgb[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imgb[0].subresourceRange.levelCount = 1;
	imgb[0].subresourceRange.layerCount = 1;

	// also make sure we can write our target - bring it into general state.
	imgb[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	imgb[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	imgb[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // discard; will overwrite
	imgb[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	imgb[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgb[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	imgb[1].image = blur.target;
	imgb[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imgb[1].subresourceRange.levelCount = 1;
	imgb[1].subresourceRange.layerCount = 1;

	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		0, 0, NULL, 0, NULL, 2, imgb);

	// bind state
	dev.dispatch.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, blur.blurPipe);
	dev.dispatch.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
		blur.pipeLayout, 0u, 1u, &rbuf.input, 0u, nullptr);

	float pcr[2] = {0.5f, 2.f};
	dev.dispatch.CmdPushConstants(cb, blur.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
		0u, 8u, pcr);

	auto gx = ceilDivide(blur.size.width / 2, 8);
	auto gy = ceilDivide(blur.size.height / 2, 8);
	dev.dispatch.CmdDispatch(cb, gx, gy, 1u);

	// kawase blur iterations
	// https://software.intel.com/content/www/us/en/develop/blogs/an-investigation-of-fast-real-time-gpu-based-image-blur-algorithms.html
	// https://gist.github.com/pkonecki/4f80114b192e60dead65e2897fd1bc0d
	auto srcID = 0u;
	auto dstID = 1u;

	VkImageMemoryBarrier targetBarriers[2];
	targetBarriers[0] = imgb[0];
	targetBarriers[1] = imgb[1];

	targetBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	targetBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	targetBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	targetBarriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	targetBarriers[0].image = blur.target;

	auto dispatch = [&](float scale) {
		float pcr[2] = {scale, 1.f};

		targetBarriers[0].subresourceRange.baseArrayLayer = srcID;
		targetBarriers[1].subresourceRange.baseArrayLayer = dstID;

		dev.dispatch.CmdPipelineBarrier(cb,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			0, 0, NULL, 0, NULL, 2, targetBarriers);

		dev.dispatch.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
			blur.pipeLayout, 0u, 1u, &blur.steps[srcID], 0u, nullptr);
		dev.dispatch.CmdPushConstants(cb, blur.pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
			0u, 8u, pcr);
		dev.dispatch.CmdDispatch(cb, gx, gy, 1u);

		std::swap(srcID, dstID);
	};

	// NOTE: strictly speaking none of the known kernels but good enough
	constexpr auto blurSteps = 10u;
	static_assert(blurSteps % 2 == 0u); // our ping-ponging relies on this
	for(auto i = 0u; i < blurSteps; ++i) {
		dispatch(float(i) + 1.5f);
	}

	// we swap an even number of times, meaning that the final result
	// is in srcID = 0.
	// Also return the swapchain image into presentSrc
	imgb[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	imgb[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	imgb[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	imgb[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

	imgb[1] = targetBarriers[0];
	imgb[1].subresourceRange.baseArrayLayer = srcID;
	dev.dispatch.CmdPipelineBarrier(cb,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, NULL, 0, NULL, 2, imgb);
}

void destroy(GuiBlur& blur) {
	if(!blur.dev) {
		return;
	}

	auto& dev = *blur.dev;
	destroyBuffers(blur, dev.dsPool);

	dev.dispatch.DestroyPipeline(dev.handle, blur.blurPipe, nullptr);
	dev.dispatch.DestroyPipelineLayout(dev.handle, blur.pipeLayout, nullptr);
	dev.dispatch.DestroyDescriptorSetLayout(dev.handle, blur.dsLayout, nullptr);
	dev.dispatch.DestroySampler(dev.handle, blur.sampler, nullptr);

	blur.blurPipe = {};
	blur.pipeLayout = {};
	blur.dsLayout = {};
}

} // namespace vil
