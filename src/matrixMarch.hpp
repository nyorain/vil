#pragma once

#include <util/linalloc.hpp>
#include <memory>

namespace vil {

// Implementation of the matrix march algorithm, developed for vil.
// It implements the fuzzy longest common subsequence (FLCS) problem,
// where (compared to the common LCS) problem, you work with
// match values in range [0, 1] instead of binary equality. Most
// fast (i.e. better than the trivial O(n^2)) solutions to LCS
// are firmly based on the binary equality and can't easily be
// extended to match values. Some even depend on a finite alphabet
// in the sequences.
//
// The algorithm has a worst-case runtime O(n^2) where n is the
// maximum number of elements in the given sequences. But in the case
// of mostly similar sequences, it will be ~O(n).
// The idea (and implementation) of the algorithm can be described
// as a best-path finding through the lazily evaluated matching matrix.
// Memory consumption is currently always O(n^2) since this was never
// a big problem. It could be reduced, being ~O(n) for the well-matching
// cases as well.
//
// In vil, we need this for command hierachy matching, associating
// commands between different frames and submissions.
// Due to the hierachical nature of our matching (and the way applications
// usually submit very similar workloads in each frame), we are interested
// in making the case of similar sequences fast.
struct MatchMatrixMarch {
	struct ResultMatch {
		u32 i;
		u32 j;
		float matchVal;
	};

	struct Result {
		span<ResultMatch> matches;
		float totalMatch;
	};

	struct EvalMatch {
		float eval {-1.f};
		float best {-1.f};
	};

	using Matcher = std::function<float(u32 i, u32 j)>;

	MatchMatrixMarch(u32 width, u32 height, LinAllocator& alloc, Matcher matcher);
	Result run();

	// Returns false if there's nothing to do anymore.
	bool step();

private:
	struct Candidate {
		u32 i;
		u32 j;
		float score;

		Candidate* prev {};
		Candidate* next {};
	};

	struct CandidateQueue {
		CandidateQueue(LinAllocator& la) : tms(la) {
			queue.next = &queue;
			queue.prev = &queue;

			freeList.next = &freeList;
			freeList.prev = &freeList;
		}

		LinAllocator& tms;

		Candidate queue; // linked list, anchor
		Candidate freeList; // linked list, anchor

		void insert(u32 i, u32 j, float score);
		Candidate pop();
		Candidate peek();
		void prune(float minScore);
		bool empty() const { return queue.next == &queue; }

		float metric(const Candidate& c) const;
	};

	u32 width_;
	u32 height_;
	LinAllocator& alloc_;
	Matcher matcher_;
	span<EvalMatch> matchMatrix_; // lazily evaluated
	float bestMatch_ {};
	std::pair<u32, u32> bestRes_ {};
	CandidateQueue queue_;

	// debug functionality
	u32 numEvals_ {};
	u32 numSteps_ {};
};

} // namespace vil
