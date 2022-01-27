#include "common.hpp"
#include <array>

TEST(bufferCreation) {
	auto& setup = getSetup();

	VkBuffer buf;
	VkDeviceMemory mem;

	VkBufferCreateInfo bci {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	bci.size = 1024u;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
		VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VK_CHECK(vkCreateBuffer(setup.dev, &bci, nullptr, &buf));

	VkMemoryRequirements memReqs;
	vkGetBufferMemoryRequirements(setup.dev, buf, &memReqs);

	VkMemoryAllocateInfo mai {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = memReqs.size;
	mai.memoryTypeIndex = findLSB(memReqs.memoryTypeBits);
	VK_CHECK(vkAllocateMemory(setup.dev, &mai, nullptr, &mem));
	VK_CHECK(vkBindBufferMemory(setup.dev, buf, mem, 0u));

	vkDestroyBuffer(setup.dev, buf, nullptr);
	vkFreeMemory(setup.dev, mem, nullptr);
}

TEST(imageCreation) {
	auto& setup = getSetup();

	VkImage img;
	VkDeviceMemory mem;

	VkImageCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.arrayLayers = 1u;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.extent = {1024u, 1024u, 1u};
	ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	ici.mipLevels = 10u;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VK_CHECK(vkCreateImage(setup.dev, &ici, nullptr, &img));

	VkMemoryRequirements memReqs;
	vkGetImageMemoryRequirements(setup.dev, img, &memReqs);

	VkMemoryAllocateInfo mai {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = memReqs.size;
	mai.memoryTypeIndex = findLSB(memReqs.memoryTypeBits);
	VK_CHECK(vkAllocateMemory(setup.dev, &mai, nullptr, &mem));
	VK_CHECK(vkBindImageMemory(setup.dev, img, mem, 0u));

	// teardown
	vkDestroyImage(setup.dev, img, nullptr);
	vkFreeMemory(setup.dev, mem, nullptr);
}

TEST(rawCommands) {
	auto& setup = getSetup();

	// setup pool
	VkCommandPoolCreateInfo cpi {};
	cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = setup.qfam;
	VkCommandPool cmdPool;
	VK_CHECK(vkCreateCommandPool(setup.dev, &cpi, nullptr, &cmdPool));

	// setup cb
	VkCommandBufferAllocateInfo cbai {};
	cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandBufferCount = 2u;
	cbai.commandPool = cmdPool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkCommandBuffer cbs[2];
	VK_CHECK(vkAllocateCommandBuffers(setup.dev, &cbai, cbs));

	// empty cb test
	{
		auto cb = cbs[0];

		VkCommandBufferBeginInfo beginInfo {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		VK_CHECK(vkBeginCommandBuffer(cb, &beginInfo));

		VK_CHECK(vkEndCommandBuffer(cb));
	}

	// command recording
	VkBuffer buf;
	VkDeviceMemory mem;

	{
		auto cb = cbs[1];

		VkCommandBufferBeginInfo beginInfo {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		VK_CHECK(vkBeginCommandBuffer(cb, &beginInfo));

		// Create buffer
		VkBufferCreateInfo bci {};
		bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		bci.size = 1024u;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VK_CHECK(vkCreateBuffer(setup.dev, &bci, nullptr, &buf));

		VkMemoryRequirements memReqs;
		vkGetBufferMemoryRequirements(setup.dev, buf, &memReqs);

		VkMemoryAllocateInfo mai {};
		mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize = memReqs.size;
		mai.memoryTypeIndex = findLSB(memReqs.memoryTypeBits);
		VK_CHECK(vkAllocateMemory(setup.dev, &mai, nullptr, &mem));

		VK_CHECK(vkBindBufferMemory(setup.dev, buf, mem, 0u));

		// Sample buffer commands
		vkCmdFillBuffer(cb, buf, 0u, 32u, 0xC0DE00FF);

		VkBufferCopy region;
		region.srcOffset = 0u;
		region.dstOffset = 32;
		region.size = 32u;
		vkCmdCopyBuffer(cb, buf, buf, 1, &region);

		u32 someData[32] {};
		vkCmdUpdateBuffer(cb, buf, 64, sizeof(someData), someData);

		VK_CHECK(vkEndCommandBuffer(cb));
	}

	// TODO: test some image commands
	// TODO: test descriptor binding, push constants
	// 	test invalidation rules.
	// TODO: render pass (+ imageless, + dynamic rendering), drawing
	// 	also dynamic state, pipeline binding
	// TODO: dispatch commands

	// create fence
	VkFenceCreateInfo fci {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence;
	VK_CHECK(vkCreateFence(setup.dev, &fci, nullptr, &fence));

	// submit
	VkSubmitInfo si {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 2u;
	si.pCommandBuffers = cbs;
	VK_CHECK(vkQueueSubmit(setup.queue, 1u, &si, fence));

	VK_CHECK(vkWaitForFences(setup.dev, 1u, &fence, true, UINT64_MAX));

	// teardown
	vkDestroyFence(setup.dev, fence, nullptr);
	vkDestroyBuffer(setup.dev, buf, nullptr);
	vkFreeMemory(setup.dev, mem, nullptr);
	vkDestroyCommandPool(setup.dev, cmdPool, nullptr);
}

TEST(descriptors) {
	auto& setup = getSetup();

	// ds pool
	auto poolSizes = std::array {
		VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 10},
	};

	VkDescriptorPoolCreateInfo dci {};
	dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dci.pPoolSizes = poolSizes.data();
	dci.poolSizeCount = poolSizes.size();
	dci.maxSets = 5u;
	VkDescriptorPool dsPool;
	VK_CHECK(vkCreateDescriptorPool(setup.dev, &dci, nullptr, &dsPool));

	// layout
	auto bindings = std::array {
		VkDescriptorSetLayoutBinding {0u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			2u, VK_SHADER_STAGE_ALL, nullptr},
	};

	VkDescriptorSetLayoutCreateInfo lci {};
	lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	lci.bindingCount = 1u;
	lci.pBindings = bindings.data();
	lci.bindingCount = bindings.size();
	VkDescriptorSetLayout dsLayout;
	VK_CHECK(vkCreateDescriptorSetLayout(setup.dev, &lci,
		nullptr, &dsLayout));

	// allocate 5 sets; just enough to exhaust the resources
	std::array layouts = {dsLayout, dsLayout, dsLayout, dsLayout, dsLayout};

	VkDescriptorSet sets[5];
	VkDescriptorSetAllocateInfo dsai {};
	dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	dsai.descriptorPool = dsPool;
	dsai.pSetLayouts = layouts.data();
	dsai.descriptorSetCount = layouts.size();
	VK_CHECK(vkAllocateDescriptorSets(setup.dev, &dsai, sets));

	// TODO: test out_of_pool_memory condition
	// TODO: test FreeDescriptors
	// TODO: test descriptor updates
	// TODO: test descriptor indexing extension

	// teardown
	vkDestroyDescriptorPool(setup.dev, dsPool, nullptr);
	vkDestroyDescriptorSetLayout(setup.dev, dsLayout, nullptr);
}
