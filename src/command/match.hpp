#pragma once

#include <fwd.hpp>
#include <vector>
#include <cstring>

namespace vil {

// Represents the result of a matching operation.
// The effective matching value is 'match/total' but having both values
// gives additional information for further processing.
// Invariant: For common objects, match <= total.
struct Matcher {
	float match {}; // sum of weights of comparisons that matched
	float total {}; // sum of weights of all comparisons

	// Special constant to signal that we are certain this isn't a match.
	static Matcher noMatch() { return {0.f, -1.f}; }
};

Matcher match(const ImageView& a, const ImageView& b);
Matcher match(const BufferView& a, const BufferView& b);
Matcher match(const Sampler& a, const Sampler& b);

// Matches the two given descriptor states, expects them to have
// the same layout.
Matcher match(const DescriptorStateRef& a, const DescriptorStateRef& b);

struct CommandSectionMatch {
	Matcher match; // including all children
	const ParentCommand* a {};
	const ParentCommand* b {};
	span<CommandSectionMatch> children;
};

// Recursively matches the (sub-)hierarchies given by rootA with rootB.
// Instead of matching each and every command, we only match sections
// (labels, render passes).
// The spans in the returned section are alloced from 'tms'.
CommandSectionMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
	const ParentCommand& rootA, const ParentCommand& rootB);

struct CommandRecordMatch {
	Matcher match;
	const CommandRecord* a {};
	const CommandRecord* b {};
	span<CommandSectionMatch> matches;
};

struct FrameSubmissionMatch {
	Matcher match;
	const FrameSubmission* a {};
	const FrameSubmission* b {};
	span<CommandRecordMatch> matches;
};

struct FrameMatch {
	Matcher match;
	span<FrameSubmissionMatch> matches;
};

FrameSubmissionMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
	const FrameSubmission& a, const FrameSubmission& b);
FrameMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
	span<const FrameSubmission>, span<const FrameSubmission>);

struct FindResult {
	std::vector<const Command*> hierarchy;
	float match;
};

// Tries to find the an equivalent of 'dstHierachyToFind' in the command
// sequence from 'srcRoot'.
FindResult find(const RootCommand& srcRoot, span<const Command*> dstHierachyToFind,
	const CommandDescriptorSnapshot& dstDescriptors, float threshold = 0.0);

// Matcher utility
template<typename T>
bool add(Matcher& m, const T& a, const T& b, float weight = 1.f) {
	auto same = (a == b);
	m.total += weight;
	m.match += same ? weight : 0.f;
	return same;
}

template<typename T>
void addMemcmp(Matcher& m, const T& a, const T& b, float weight = 1.f) {
	m.total += weight;
	m.match += std::memcmp(&a, &b, sizeof(T)) == 0 ? weight : 0.f;
}

template<typename T>
void addNonNull(Matcher& m, T* a, T* b, float weight = 1.f) {
	m.total += weight;
	m.match += (a == b && a != nullptr) ? weight : 0.f;
}

float eval(const Matcher& m);
bool valid(const Matcher& m);

void add(Matcher& m,
	const VkImageSubresourceRange& a, const VkImageSubresourceRange& b,
	float weight = 1.f);
void add(Matcher& m,
	const VkImageSubresourceLayers& a, const VkImageSubresourceLayers& b,
	float weight = 1.f);

} // namespace vil
