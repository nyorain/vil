#pragma once

#include <fwd.hpp>
#include <vkutil/handles.hpp>
#include <commandHook/hook.hpp>
#include <util/buffmt.hpp>
#include <util/linalloc.hpp>

namespace vil {

struct PatchResult {
	LinAllocator alloc;
	span<Type::Member> captures;
	std::vector<u32> copy;
};

PatchResult patchShaderCapture(const spc::Compiler&, u32 file, u32 line);

vku::Pipeline createPatchCopy(const Pipeline& src, VkShaderStageFlagBits stage,
	span<const u32> patchedSpv);

// patchJob
enum class PatchJobState {
	started,
	canceled,
	installing,
	done,
};

struct PatchJobData {
	// TODO: won't work like this...
	IntrusiveDerivedPtr<Pipeline> pipe;
	const spc::Compiler* compiler;
	VkShaderStageFlagBits stage;
	std::string entryPoint;
	u32 file;
	u32 line;
	PatchJobState state {PatchJobState::started};
	u64 captureAddress;
	Vec3u32 captureInput {};
	// CommandHookTarget hookTarget;
};

struct PatchJobResult {
	LinAllocator alloc;
	span<Type::Member> captures;
	vku::Pipeline pipe;
	std::string error;
};

PatchJobResult patchJob(PatchJobData& data);

} // namespace
