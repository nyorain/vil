#include "../bugged.hpp"
#include <rp.hpp>
#include <nytl/span.hpp>

using namespace vil;

auto& addAttachment(RenderPassDesc& desc, bool multisampled = false) {
	auto& att = desc.attachments.emplace_back();
	att.pNext = {};
	att.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
	att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att.format = VK_FORMAT_R8G8B8A8_UNORM;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.samples = multisampled ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
	return att;
}

auto& addSubpass(RenderPassDesc& desc, span<const u32> colorAtts,
		span<const u32> resolveAtts = {}) {
	auto& subp = desc.subpasses.emplace_back();
	subp = {};
	subp.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;

	auto& colorRefs = desc.attachmentRefs.emplace_back();
	for(auto c : colorAtts) {
		auto& ref = colorRefs.emplace_back();
		ref.pNext = {};
		ref.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		ref.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ref.attachment = c;
		ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	subp.pColorAttachments = colorRefs.data();
	subp.colorAttachmentCount = colorRefs.size();

	if(!resolveAtts.empty()) {
		auto& resRefs = desc.attachmentRefs.emplace_back();
		dlg_assert(resolveAtts.size() == colorAtts.size());
		for(auto c : resolveAtts) {
			auto& resRef = resRefs.emplace_back();
			resRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
			resRef.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			resRef.attachment = c;
			resRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}

		subp.pResolveAttachments = resRefs.data();
	}

	return subp;
}

TEST(unit_rpsplit_basic) {
	RenderPassDesc desc;
	addAttachment(desc); // attachment 0
	addAttachment(desc, true); // attachment 1
	addSubpass(desc, {{0u, 1u}}); // subpass 0
	EXPECT(splittable(desc, 0u), true);

	addSubpass(desc, {{0u, 1u}}); // subpass 1
	EXPECT(splittable(desc, 0u), true);
	EXPECT(splittable(desc, 1u), true);

	addSubpass(desc, {{1u}}, {{0u}}); // subpass 2
	// can't split in subpass 0 or 1 because the resolve in subpass 2
	// would overwrite the content of attachment 0 even that that is
	// read from (e.g. blending) in subpass 0 and 1
	EXPECT(splittable(desc, 0u), false);
	EXPECT(splittable(desc, 1u), false);
	EXPECT(splittable(desc, 2u), true);
}

TEST(unit_rpsplit_single_resolve) {
	RenderPassDesc desc;
	addAttachment(desc); // attachment 0
	addAttachment(desc, true); // attachment 1
	addSubpass(desc, {{1u}}, {{0u}}); // subpass 2

	EXPECT(splittable(desc, 0u), true);
}

TEST(unit_rpsplit_input_att) {
	RenderPassDesc desc;
	addAttachment(desc); // attachment 0
	addAttachment(desc); // attachment 1
	addAttachment(desc, true); // attachment 2
	auto& subpass = addSubpass(desc, {{2u}}, {{0u}}); // subpass 2

	auto& inRef = desc.attachmentRefs.emplace_back().emplace_back();
	inRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
	inRef.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	inRef.attachment = 0u;
	inRef.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	subpass.pInputAttachments = &inRef;
	subpass.inputAttachmentCount = 1u;

	EXPECT(splittable(desc, 0u), false);
}

TEST(unit_rpsplit_unused) {
	// make that VK_ATTACHMENT_UNUSED does not cause an error, as
	// it previously did
	RenderPassDesc desc;
	addAttachment(desc, true); // attachment 0
	addAttachment(desc, true); // attachment 1
	addAttachment(desc); // attachment 2
	addAttachment(desc); // attachment 3
	addSubpass(desc, {{0u, 1u}}); // subpass 0
	EXPECT(splittable(desc, 0u), true);

	addSubpass(desc, {{0u, 1u}}); // subpass 1
	addSubpass(desc, {{1u, VK_ATTACHMENT_UNUSED}}); // subpass 2
	addSubpass(desc, {{0u, VK_ATTACHMENT_UNUSED}}, {{2u, 3u}}); // subpass 3

	EXPECT(splittable(desc, 0u), true);
	EXPECT(splittable(desc, 1u), true);
	EXPECT(splittable(desc, 2u), true);
	EXPECT(splittable(desc, 3u), true);
}
