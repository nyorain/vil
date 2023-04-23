#pragma once

#include <fwd.hpp>
#include <vector>
#include <cstring>
#include <nytl/span.hpp>
#include <util/linalloc.hpp>
#include <vk/vulkan.h>

namespace vil {

// Represents the result of a matching operation.
// The effective matching value is 'match/total' but having both values
// gives additional information for further processing.
// Invariant: For common objects, match <= total.
struct MatchVal {
	float match {}; // sum of weights of comparisons that matched
	float total {}; // sum of weights of all comparisons

	// Special constant to signal that we are certain this isn't a match.
	static MatchVal noMatch() { return {0.f, -1.f}; }
};

// Matches the two given descriptor states, expects them to have
// the same layout.
MatchVal match(MatchType, const DescriptorStateRef& a, const DescriptorStateRef& b);

struct CommandSectionMatch {
	MatchVal match; // including all children
	const ParentCommand* a {};
	const ParentCommand* b {};
	span<CommandSectionMatch> children;
};

// Recursively matches the (sub-)hierarchies given by rootA with rootB.
// Instead of matching each and every command, we only match sections
// (labels, render passes).
// The spans in the returned section are alloced from 'tms'.
CommandSectionMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
	MatchType, const ParentCommand& rootA, const ParentCommand& rootB);

struct CommandRecordMatch {
	MatchVal match;
	const CommandRecord* a {};
	const CommandRecord* b {};
	span<CommandSectionMatch> matches;
};

struct FrameSubmissionMatch {
	MatchVal match;
	const FrameSubmission* a {};
	const FrameSubmission* b {};
	span<CommandRecordMatch> matches;
};

struct FrameMatch {
	MatchVal match;
	span<FrameSubmissionMatch> matches;
};

FrameSubmissionMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
	MatchType, const FrameSubmission& a, const FrameSubmission& b);
FrameMatch match(LinAllocScope& retMem, LinAllocScope& localMem,
	MatchType, span<const FrameSubmission>, span<const FrameSubmission>);

struct FindResult {
	std::vector<const Command*> hierarchy;
	float match;
};

// Tries to find the an equivalent of 'dstHierachyToFind' in the command
// sequence from 'srcRoot'.
FindResult find(MatchType,
	const ParentCommand& srcRoot,
	span<const Command*> dstHierarchyToFind,
	const CommandDescriptorSnapshot& dstDescriptors,
	float threshold = 0.0);

// Matcher utility
template<typename T>
bool add(MatchVal& m, const T& a, const T& b, float weight = 1.f) {
	auto same = (a == b);
	m.total += weight;
	m.match += same ? weight : 0.f;
	return same;
}

inline bool noMatch(const MatchVal& m) {
	return m.total < 0.f;
}

inline void add(MatchVal& m, const MatchVal& other, float weight = 1.0) {
	dlg_assert(!noMatch(other) && !noMatch(m));
	m.total += other.total * weight;
	m.match += other.match * weight;
}

template<typename T>
void addMemcmp(MatchVal& m, const T& a, const T& b, float weight = 1.f) {
	m.total += weight;
	m.match += std::memcmp(&a, &b, sizeof(T)) == 0 ? weight : 0.f;
}

template<typename T>
void addNonNull(MatchVal& m, T* a, T* b, float weight = 1.f) {
	m.total += weight;
	m.match += (a == b && a != nullptr) ? weight : 0.f;
}

bool addNonEmpty(MatchVal&, std::string_view str1, std::string_view str2, float weight = 1.f);

float eval(const MatchVal& m);
bool valid(const MatchVal& m);

void add(MatchVal& m,
	const VkImageSubresourceRange& a, const VkImageSubresourceRange& b,
	float weight = 1.f);
void add(MatchVal& m,
	const VkImageSubresourceLayers& a, const VkImageSubresourceLayers& b,
	float weight = 1.f);

MatchVal match(const Command& a, const Command& b, MatchType matchType);

} // namespace vil
