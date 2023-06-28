#include <vkutil/pipe.hpp>
#include <vkutil/spirv_reflect.h>
#include <nytl/scope.hpp>
#include <device.hpp>

namespace vil::vku {

constexpr auto shaderEntryPointName = "main";

// Reflection
void assertSuccess(SpvReflectResult res, bool allowNotFound = false) {
	dlg_assert(res == SPV_REFLECT_RESULT_SUCCESS ||
		(allowNotFound && res == SPV_REFLECT_RESULT_ERROR_ELEMENT_NOT_FOUND));
}

template<typename V>
void resizeAtLeast(V& vec, std::size_t size) {
	if(vec.size() < size) {
		vec.resize(size);
	}
}

bool needsSampler(VkDescriptorType dsType) {
	switch(dsType) {
		case VK_DESCRIPTOR_TYPE_SAMPLER:
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			return true;
		default:
			return false;
	}
}

VkDescriptorType toVk(SpvReflectDescriptorType type) {
	return static_cast<VkDescriptorType>(type);
}

struct ReflectStage {
	span<const u32> spv;
	VkShaderStageFlagBits stage;
};

struct ReflectResult {
	PipelineLayout pipeLayout;
};

PipelineLayout reflectPipeState(Device& dev,
		span<const ReflectStage> stages, const PipeCreator& creator,
		std::array<DynDsLayout, maxNumDescriptorSets>& dynLayouts,
		std::string_view name, bool forceOneDynamic) {
	ZoneScoped;

	// NOTE: relevant for validation layers. They have issues with using
	// large values here.
	auto maxVarBindingsCount = 16 * 1024;

	// descriptor set layouts
	struct LayoutDesc {
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		std::vector<VkDescriptorBindingFlags> flags;
		VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo;
	};

	std::array<LayoutDesc, maxNumDescriptorSets> dynBindings;
	std::array<VkDescriptorSetLayout, maxNumDescriptorSets> setLayouts {};
	std::vector<VkPushConstantRange> pcrs;
	std::vector<std::unique_ptr<VkSampler[]>> keepAliveSamplers;

	for(auto i = 0u; i < maxNumDescriptorSets; ++i) {
		setLayouts[i] = creator.layout(i);
	}

	for(auto& stage : stages) {
		dlg_assert(stage.stage != VkShaderStageFlagBits {});
		dlg_assert(!stage.spv.empty());

		auto& spv = stage.spv;
		SpvReflectShaderModule refl {};
		assertSuccess(spvReflectCreateShaderModule(spv.size() * sizeof(u32), spv.data(), &refl));

		auto modGuard = nytl::ScopeGuard([&]{
			spvReflectDestroyShaderModule(&refl);
		});

		// descriptor sets
		for(auto i = 0u; i < refl.descriptor_set_count; ++i) {
			auto& set = refl.descriptor_sets[i];
			dlg_assertm(set.set < maxNumDescriptorSets,
				"Descriptor set {} is too high, not supported", set.set);

			if(setLayouts[set.set]) {
				continue;
			}

			for(auto i = 0u; i < set.binding_count; ++i) {
				auto& binding = *set.bindings[i];

				// NOTE: keep in mind that spec constants are not supported
				// by spirv-reflect, even though they might be used e.g.
				// to define the array size of a binding.

				// variable descriptor count array
				auto elemCount = binding.count;
				if(binding.count == 0u) {
					// NOTE: unexpected atm
					dlg_trace("Found binding with variable descriptor count");
					// dlg_assertm(hasVarCount, "Device does not support variable "
					// 	"descriptor count but shader uses it");

					elemCount = maxVarBindingsCount;
					resizeAtLeast(dynBindings[set.set].flags, binding.binding + 1);
					dynBindings[set.set].flags[set.set] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
				}

				resizeAtLeast(dynBindings[set.set].bindings, binding.binding + 1);
				auto& info = dynBindings[set.set].bindings[binding.binding];

				if(info.stageFlags) { // was already seen before
					dlg_assert(info.binding == binding.binding);
					dlg_assertm((info.stageFlags & stage.stage) == 0,
						"{}: Duplicate shader binding {}", name, binding.binding);
					info.descriptorCount = std::max(info.descriptorCount, elemCount);
					info.stageFlags |= stage.stage;
					dlg_assertm(info.descriptorType == toVk(binding.descriptor_type),
						"{}: pipeline shader stage bindings don't match", name);
				} else {
					info.binding = binding.binding;
					info.descriptorCount = elemCount;
					info.stageFlags = stage.stage;
					info.descriptorType = toVk(binding.descriptor_type);
					if(needsSampler(info.descriptorType)) {
						std::unique_ptr<VkSampler[]> samplersPtr;
						auto samplers = creator.immutableSamplers(set.set,
							binding.binding, elemCount, samplersPtr);
						if(samplers) {
							info.pImmutableSamplers = samplers;
							if(samplersPtr) {
								keepAliveSamplers.push_back(std::move(samplersPtr));
							}
						}
					}
				}
			}
		}

		// query push constants
		SpvReflectResult res;
		auto blockVar = spvReflectGetEntryPointPushConstantBlock(&refl,
			shaderEntryPointName, &res);
		assertSuccess(res, true);

		if(blockVar) {
			auto& pcr = pcrs.emplace_back();
			pcr.offset = blockVar->offset;
			pcr.size = blockVar->size;
			pcr.stageFlags = stage.stage;
		}
	}

	u32 firstUnusedSetID = u32(-1);
	for(auto i = 0u; i < maxNumDescriptorSets; ++i) {
		if(firstUnusedSetID != u32(-1)) {
			// Here, we have something like this
			// - set 0: known (either inferred or given statically)
			// - set 1: empty (not used in any shader, not given statically)
			// - set 2: known (either inferred or given statically)
			// That's a problem since pipeline layouts can't have holes.
			// We could create an empty ds layout but i can't think
			// of a case where this is actually what we want.
			// We should just remove the empty set in the shader. If we have
			// it to not disturb another binding, we *must* provide its
			// layout statically.
			dlg_assertm(!setLayouts[i] && dynBindings[i].bindings.empty(),
				"Hole in pipeline layout, descriptor set {}", firstUnusedSetID);
			continue;
		}

		if(setLayouts[i]) {
			continue;
		}

		auto& desci = dynBindings[i];
		const auto needSet =
			// there are dynamic bindings in this set
			!desci.bindings.empty() ||
			// we need to create at least one dynamic set and haven't yet
			forceOneDynamic ||
			// the creator explicitly requests us to create an empty dummy layout
			i < creator.minNumberSets();
		if(!needSet) {
			firstUnusedSetID = i;
			continue;
		}

		if(!desci.flags.empty()) {
			desci.flags.resize(desci.bindings.size());
		}

		// Erase unused bindings.
		// Note that numbering here does not matter, the bindingID
		// is explicitly encoded in the vk::DescriptorSetLayoutBinding
		for(auto j = 0u; j < desci.bindings.size();) {
			if(desci.bindings[j].descriptorCount == 0u) {
				desci.bindings.erase(desci.bindings.begin() + j);
				if(!desci.flags.empty()) {
					desci.flags.erase(desci.flags.begin() + j);
				}
			} else {
				++j;
			}
		}

		creator.initDynDsLayout(dev, i, dynLayouts[i], dynBindings[i].bindings,
			dynBindings[i].flags);

		// TODO: name!
		// auto dslName = dlg::format("{}[{}]", name, i);
		// vpp::nameHandle(dynLayouts[i], dslName);

		dlg_assert(dynLayouts[i].vkHandle());
		setLayouts[i] = dynLayouts[i].vkHandle();

		// we have a dynamic set slot, don't need to create another
		forceOneDynamic = false;
	}

	auto sl = span(setLayouts.data(), firstUnusedSetID);
	auto ret = creator.createPipeLayout(dev, sl, pcrs);

	// TODO: name!
	// auto plName = dlg::format("{}", name);
	// vpp::nameHandle(ret, plName);

	return ret;
}

// PipeInfoProvider
PipelineLayout PipeCreator::createPipeLayout(Device& dev,
		span<const VkDescriptorSetLayout> dsLayouts,
		span<const VkPushConstantRange> pcrs) const {
	return PipelineLayout(dev, dsLayouts, pcrs);
}

void PipeCreator::initDynDsLayout(Device& dev, unsigned set, DynDsLayout& dsLayout,
		span<const VkDescriptorSetLayoutBinding> bindings,
		span<const VkDescriptorBindingFlags> flags) const {
	(void) set;
	VkDescriptorSetLayoutCreateInfo dli {};
	dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dli.bindingCount = bindings.size();
	dli.pBindings = bindings.data();

	VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo {};
	if(!flags.empty()) {
		flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
		flagsInfo.bindingCount = flags.size();
		flagsInfo.pBindingFlags = flags.data();
		dli.pNext = &flagsInfo;
	}

	dsLayout.init(dev, dli, "");
}

std::unique_ptr<PipeCreator> PipeCreator::graphics(const VkGraphicsPipelineCreateInfo& info,
		DescriptorInfo dsInfo,
		std::unordered_map<VkShaderStageFlagBits, SpecializationInfo> spec) {

	struct Impl : public PipeCreator {
		mutable VkGraphicsPipelineCreateInfo gpi;
		std::unordered_map<VkShaderStageFlagBits, SpecializationInfo> spec;
		DescriptorInfo dsInfo;

		VkDescriptorSetLayout layout(unsigned set) const override {
			if(set < dsInfo.dsLayouts.size()) {
				return dsInfo.dsLayouts[set];
			}

			return {};
		}

		Pipeline create(Device& dev, VkPipelineLayout layout, VkPipelineCache pc,
				span<const VkPipelineShaderStageCreateInfo> stages) const override {
			dlg_assert(stages.size() >= 1);
			gpi.layout = layout;
			gpi.pStages = stages.data();
			gpi.stageCount = stages.size();
			return Pipeline(dev, gpi, pc);
		}

		const VkSampler* immutableSamplers(unsigned set, unsigned binding,
				unsigned count, std::unique_ptr<VkSampler[]>& keepAlive) const override {
			const VkSampler* ret {};
			auto it = dsInfo.immutableSamplerMap.find(BindingID {set, binding});
			if(it != dsInfo.immutableSamplerMap.end()) {
				ret = &it->second;
			}

			if(!ret && dsInfo.immutableSampler) {
				ret = &dsInfo.immutableSampler;
			}

			if(!ret) {
				return nullptr;
			}

			if(count == 1u) {
				return ret;
			} else {
				keepAlive = std::make_unique<VkSampler[]>(count);
				std::fill_n(keepAlive.get(), count, *ret);
				return keepAlive.get();
			}
		}

		unsigned minNumberSets() const override {
			return dsInfo.minNumSets;
		}

		Impl(const VkGraphicsPipelineCreateInfo& xgpi,
			std::unordered_map<VkShaderStageFlagBits, SpecializationInfo> xspec,
			DescriptorInfo xdsInfo) :
				gpi(std::move(xgpi)), spec(std::move(xspec)), dsInfo(std::move(xdsInfo)) {
		}
	};

	return std::make_unique<Impl>(std::move(info), spec, std::move(dsInfo));
}

std::unique_ptr<PipeCreator> PipeCreator::compute(DescriptorInfo dsInfo,
		SpecializationInfo spec) {
	struct Impl : public PipeCreator {
		SpecializationInfo spec;
		DescriptorInfo dsInfo;

		VkDescriptorSetLayout layout(unsigned set) const override {
			if(set < dsInfo.dsLayouts.size()) {
				return dsInfo.dsLayouts[set];
			}

			return {};
		}

		Pipeline create(Device& dev, VkPipelineLayout layout, VkPipelineCache cache,
				span<const VkPipelineShaderStageCreateInfo> stages) const override {
			dlg_assert(stages.size() == 1);
			auto speci = spec.info();

			VkComputePipelineCreateInfo cpi;
			cpi.layout = layout;
			cpi.stage = stages[0];
			cpi.stage.pSpecializationInfo = &speci;
			return Pipeline(dev, cpi, cache);
		}

		const VkSampler* immutableSamplers(unsigned set, unsigned binding,
				unsigned count, std::unique_ptr<VkSampler[]>& keepAlive) const override {
			const VkSampler* ret {};
			auto it = dsInfo.immutableSamplerMap.find(BindingID {set, binding});
			if(it != dsInfo.immutableSamplerMap.end()) {
				ret = &it->second;
			}

			if(!ret && dsInfo.immutableSampler) {
				ret = &dsInfo.immutableSampler;
			}

			if(!ret) {
				return nullptr;
			}

			if(count == 1u) {
				return ret;
			} else {
				keepAlive = std::make_unique<VkSampler[]>(count);
				std::fill_n(keepAlive.get(), count, *ret);
				return keepAlive.get();
			}
		}

		unsigned minNumberSets() const override {
			return dsInfo.minNumSets;
		}

		Impl(SpecializationInfo xspec, DescriptorInfo xdsInfo) :
			spec(std::move(xspec)), dsInfo(std::move(xdsInfo)) {
		}
	};

	return std::make_unique<Impl>(std::move(spec), std::move(dsInfo));
}

void DynamicPipe::init(Device& dev, span<const Stage> inStages,
		std::unique_ptr<PipeCreator> creator, std::string name) {
	dlg_assert(!inStages.empty());
	std::vector<VkPipelineShaderStageCreateInfo> stages;
	std::vector<ReflectStage> reflStages;
	std::vector<VkShaderModule> modules;

	for(auto& stage : inStages) {
		VkShaderModuleCreateInfo sci {};
		sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		sci.codeSize = stage.spirv.size() * 4;
		sci.pCode = stage.spirv.data();

		auto& shaderModule = modules.emplace_back();
		dev.dispatch.CreateShaderModule(dev.handle, &sci, nullptr, &shaderModule);

		auto& dst = stages.emplace_back();
		dst.stage = stage.stage;
		dst.module = shaderModule;
		dst.pName = shaderEntryPointName;

		auto& refl = reflStages.emplace_back();
		refl.spv = stage.spirv;
		refl.stage = dst.stage;
	}

	constexpr auto forceOneDynamicDs = true;
	pipeLayout_ = reflectPipeState(dev, reflStages, *creator,
		dynDsLayouts_, name, forceOneDynamicDs);

	ZoneScopedN("vkCreatePipeline");
	pipe_ = creator->create(dev, pipeLayout_.vkHandle(), {}, stages);
}

} // namespace
