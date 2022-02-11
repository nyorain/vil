#pragma once

// TODO: clean up. And rename to command/match.hpp?
// It's not so much about descriptions, it's about matching commands/records.
// And add more, proper documentation.

#include <fwd.hpp>
#include <ds.hpp>
#include <vector>
#include <string>
#include <functional>

namespace vil {

struct FindResult {
	std::vector<const Command*> hierachy;
	float match;
};

// Assumes that 'root' is a command tree where all handles are still valid.
FindResult find(const Command* srcRoot, span<const Command*> dstHierachyToFind,
	const CommandDescriptorSnapshot& dstDescriptors, float threshold = 0.0);

// WIP
struct RecordBatch;

struct SectionMatch {
	float match {}; // including children match
	const ParentCommand* a {};
	const ParentCommand* b {};
	span<SectionMatch> children;
};

// TODO: to be removed when we have everything else in place.
void annotateRelIDLegacy(Command*);

// TODO: match values currently not normalized to [0, 1]

struct RecordMatch {
	float match {};
	const CommandRecord* a {};
	const CommandRecord* b {};
	span<SectionMatch> matches;
};

struct BatchMatch {
	float match {};
	const RecordBatch* a {};
	const RecordBatch* b {};
	span<RecordMatch> matches; // TODO: currently reversed ordered
};

struct MatchResult {
	float match {};
	span<BatchMatch> matches; // TODO: currently reversed ordered
};

MatchResult match(ThreadMemScope&, span<const RecordBatch> a, span<const RecordBatch> b);

} // namespace vil
