#pragma once

#include <vkutil/dynds.hpp>
#include <nytl/bytes.hpp>

// Inspired by iro/pipeline. But we don't want dynamic shader reloading
// in vil, we want to explicitly have the spirv compiled into the binary

namespace vil::vku {

constexpr auto maxNumDescriptorSets = 4u;

// SpecializationInfo
struct SpecializationInfo {
	DynWriteBuf data;
	std::vector<VkSpecializationMapEntry> entries;

	template<typename... Args>
	static SpecializationInfo create(const Args&... args) {
		SpecializationInfo ret;
		(ret.add(args), ...);
		return ret;
	}

	template<typename T>
	std::enable_if_t<nytl::BytesConvertible<T>> add(const T& obj) {
		add(entries.empty() ? 0u : entries.back().constantID, data.size(), obj);
	}

	template<typename T>
	std::enable_if_t<nytl::BytesConvertible<T>> add(
			unsigned constantID, unsigned offset, const T& obj) {
		auto& entry = entries.emplace_back();
		entry.constantID = constantID;
		entry.offset = offset;
		entry.size = sizeof(obj);
		nytl::write(data, obj);
	}

	VkSpecializationInfo info() const {
		VkSpecializationInfo spec;
		spec.dataSize = data.size();
		spec.pData = data.data();
		spec.mapEntryCount = entries.size();
		spec.pMapEntries = entries.data();
		return spec;
	}
};

struct BindingID {
	u32 set;
	u32 binding;
};

inline bool operator==(BindingID a, BindingID b) {
	return a.set == b.set && a.binding == b.binding;
}
inline bool operator!=(BindingID a, BindingID b) {
	return a.set != b.set || a.binding != b.binding;
}

struct PairHash {
	template <typename T>
	std::size_t operator()(const T& x) const {
		auto& [first, second] = x;
		using A = std::remove_reference_t<std::remove_cv_t<decltype(first)>>;
		using B = std::remove_reference_t<std::remove_cv_t<decltype(second)>>;
		return std::hash<A>()(first) ^ std::hash<B>()(second);
	}
};

struct PipeDescriptorInfo {
	// Immutable sampler set for all bindings that don't have an entry
	// in immutableSamplerMap.
	VkSampler immutableSampler {};
	// Predefined external descriptor set layouts
	std::vector<VkDescriptorSetLayout> dsLayouts {};
	std::unordered_map<BindingID, VkSampler, PairHash> immutableSamplerMap {};
	// The minimum number of sets to add to the pipeline layout.
	// The sets that don't have a predefined layout (via this->dsLayouts)
	// and don't have any bindings in the pipeline will simply be
	// created empty.
	unsigned minNumSets {};
};

// Provides information for pipeline creation via DynamicPipe.
// The member functions might be called from any thread (but never
// from more than one at a time), the object is expected
// to be self-contained.
struct PipeCreator {
	using DescriptorInfo = PipeDescriptorInfo;

	// Returns a default implementation for graphic pipelines.
	// The given sampler is used for all immutable samplers.
	// Using the immutable sampler does not work with array bindings!
	static std::unique_ptr<PipeCreator> graphics(
		const VkGraphicsPipelineCreateInfo& info,
		DescriptorInfo dsInfo = {},
		std::unordered_map<VkShaderStageFlagBits, SpecializationInfo> = {});

	// Returns default implementation for compute pipelines.
	// The given sampler is used for all immutable samplers.
	// Using the immutable sampler does not work with array bindings!
	static std::unique_ptr<PipeCreator> compute(
		DescriptorInfo dsInfo = {}, SpecializationInfo = {});

	virtual ~PipeCreator() = default;

	// Returns the pre-known, fixed DescriptorSetLayout for the gived
	// set id. If this returns null, the layout will be dynamically
	// inferred from the shaders.
	virtual VkDescriptorSetLayout layout(unsigned set) const {
		(void) set;
		return {};
	}

	// Generate the pipeline layout from the inferred descriptor set layouts
	// and push constant ranges.
	virtual PipelineLayout createPipeLayout(Device& dev,
		span<const VkDescriptorSetLayout> dsLayouts,
		span<const VkPushConstantRange> pcrs) const;

	// Must initialize the given DynDsLayout for the dynamic set.
	// Gets the bindings parsed from the pipeline.
	virtual void initDynDsLayout(Device& dev, unsigned set, DynDsLayout&,
		span<const VkDescriptorSetLayoutBinding> bindings,
		span<const VkDescriptorBindingFlags> flags) const;

	// Allows to add immutable samplers to the reflected descriptor set
	// layout bindings for the dynamic set.
	// Must always return a pointer to 'count' Samplers. If it would need to
	// allocate to create such an array, can use 'keepAlive'.
	virtual const VkSampler* immutableSamplers(unsigned set, unsigned binding,
			unsigned count, std::unique_ptr<VkSampler[]>& keepAlive) const {
		(void) set;
		(void) binding;
		(void) count;
		(void) keepAlive;
		return {};
	}

	// Returns the minimum number of sets to add to the pipeline layout.
	// The sets that don't have a predefined layout (via this->layout(i))
	// and don't have any bindings in the pipeline will simply be
	// created empty.
	virtual unsigned minNumberSets() const { return 0u; }

	// Callback to create the actual pipeline from the previously created
	// pipelineLayout and the loaded stages.
	virtual Pipeline create(Device& dev, VkPipelineLayout, VkPipelineCache,
		span<const VkPipelineShaderStageCreateInfo> stages) const = 0;
};

class DynamicPipe {
public:
	struct Stage {
		span<const u32> spirv {};
		VkShaderStageFlagBits stage {};
	};

public:
	void init(Device& dev, span<const Stage>, std::unique_ptr<PipeCreator>,
		std::string name);

	const PipelineLayout& pipeLayout() const {
		dlg_assert(pipeLayout_.vkHandle());
		return pipeLayout_;
	}

	VkPipeline pipe() const { return pipe_.vkHandle(); }

	// Returns the dynamically loaded layout at the given set slot.
	// Invalid to call this for pre-provided descriptor layouts or
	// before the pipeline was first created.
	const DynDsLayout& dynDsLayout(unsigned set) const {
		dlg_assertm(pipeLayout_.vkHandle(), "Pipeline was not created yet");
		dlg_assert(set < maxNumDescriptorSets);
		dlg_assert(dynDsLayouts_[set].vkHandle());
		return dynDsLayouts_[set];
	}
	const std::array<DynDsLayout, maxNumDescriptorSets>& dynDsLayouts() const {
		dlg_assertm(pipeLayout_.vkHandle(), "Pipeline was not created yet");
		return dynDsLayouts_;
	}

	const std::string& name() const { return name_; }

private:
	Pipeline pipe_;
	PipelineLayout pipeLayout_;
	std::array<DynDsLayout, maxNumDescriptorSets> dynDsLayouts_;
	std::string name_;
};

} // namespace
