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

struct RecordBatch;

// TODO: to be removed when we have everything else in place.
void annotateRelIDLegacy(Command*);

// TODO: not all match values currently not normalized to [0, 1]
//   or maybe rather always include a Matcher?
// TODO: propagate Matcher objects instead of float for record and batch
//   matching as well to make sure we put more weight on large
//   records/submissions aligning.

struct SectionMatch {
	float match {}; // including children match; normalized
	const ParentCommand* a {};
	const ParentCommand* b {};
	span<SectionMatch> children;
};

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

// util
Matcher match(DescriptorStateRef a, DescriptorStateRef b);
BatchMatch match(ThreadMemScope& tms, const RecordBatch& a, const RecordBatch& b);
std::pair<span<SectionMatch>, Matcher> match(ThreadMemScope& tms,
		const ParentCommand& rootA, const ParentCommand& rootB);

} // namespace vil
