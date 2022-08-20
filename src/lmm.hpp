#pragma once

#include <util/linalloc.hpp>
#include <functional>
#include <utility>
#include <set>

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
//
// NOTE: for very quick comparisons (e.g. binary int/char comparison or simple
// match retrieval), this might still be slower than the trivial algorithm.
// But if you have costly comparisons (e.g. for hierachical matching as we
// do with command buffers), this implementation can be an order of
// magnitude faster, especially when there's a strong correlation.
struct LazyMatrixMarch {
	// Describes a match between the ith sequence item in the first sequence
	// with the jth sequence item in the second sequence, with a match
	// equal to 'matchVal'.
	struct ResultMatch {
		u32 i;
		u32 j;
		float matchVal;
	};

	struct Result {
		// accumulated matching value of the best path
		float totalMatch;
		// All the matches found on the best path
		// Span lifetime depends on LinAllocator this is constructed with
		span<ResultMatch> matches;
	};

	struct HeapCand {
		u16 i;
		u16 j;
		float score;
	};

	struct HeapCandCompare {
		LazyMatrixMarch& parent;

		bool operator()(const HeapCand& a, const HeapCand& b) const {
			auto scA = parent.maxPossibleScore(a.score, a.i, a.j);
			auto scB = parent.maxPossibleScore(b.score, b.i, b.j);

			if(scA < scB) {
				return true;
			} else if(scB < scA) {
				return false;
			}

			if(a.score < b.score) {
				return true;
			} else if(b.score < a.score) {
				return false;
			}

			if(a.i < b.i) {
				return true;
			} else if(b.i < a.i) {
				return false;
			}

			return a.j < b.j;
		}
	};

	template<typename T>
	struct MyAlloc : LinearUnscopedAllocator<T> {
		using typename LinearUnscopedAllocator<T>::is_always_equal;
		using typename LinearUnscopedAllocator<T>::value_type;
		using LinearUnscopedAllocator<T>::LinearUnscopedAllocator;
		using LinearUnscopedAllocator<T>::allocate;

		void deallocate(T*, size_t) const noexcept {
			// TODO: use free-list
			// dlg_error(":(");
		}
	};

	using QSet = std::set<HeapCand, HeapCandCompare, MyAlloc<HeapCand>>;

	struct EvalMatch {
		// The result of the matcher function at this position.
		// Lazily evaluated, -1.f if it never was called
		float eval {-1.f};
		// The best path found so far to this position
		// -1.f when we never had a path here
		float best {-1.f};
		// current candidate, if any.
		// with this we can make sure there is never more than one
		// candidate per field
		QSet::const_iterator candidate; // candidates_.end() when there is none
	};

	// The function evaluating the match between the ith element in the
	// first sequence with the jth element in the second sequence.
	// Note how the LazyMatrixMarch algorithm itself never sees the sequences
	// itself, does not care about their types of properties.
	// Expected to return a matching value in range [0, 1] where 0
	// means no match and a value >0 means there's a match, returning
	// it's weight/value/importance/quality.
	// Guaranteed to be called at most once per run for each (i, j)
	// combinations so don't bother caching results.
	using Matcher = std::function<float(u32 i, u32 j)>;

	// width: length of the first sequence
	// height: length of the second sequence
	// alloc: an allocator guaranteed to outlive this
	// matcher: the matching functions holding information about the sequences
	LazyMatrixMarch(u32 width, u32 height, LinAllocator& alloc,
		Matcher matcher, float branchThreshold = 0.95);

	// Runs the algorithm to completion (can also be called if 'step' was
	// called before) and returns the best path and its matches.
	Result run();

	// Returns false if there's nothing to do anymore.
	bool step();

	// inspection
	HeapCand peekCandidate() const;
	const auto& candidates() const { return candidates_; }
	bool empty() const { return candidates_.empty(); }
	const EvalMatch& matchData(u32 i, u32 j) const {
		return matchMatrix_[width() * j + i];
	}

	u32 width() const { return width_; }
	u32 height() const { return height_; }

	// debug information
	u32 numEvals() const { return numEvals_; }
	u32 numSteps() const { return numSteps_; }

private:
	void addCandidate(float score, u32 i, u32 j, u32 addI, u32 addJ);

	HeapCand popCandidate();
	void prune(float minScore);
	EvalMatch& match(u32 i, u32 j) {
		return matchMatrix_[width() * j + i];
	}

	// util
	float maxPossibleScore(float score, u32 i, u32 j) const;
	float maxPossibleScore(const HeapCand& c) {
		return maxPossibleScore(c.score, c.i, c.j);
	}

private:
	LinAllocator& alloc_;
	u32 width_;
	u32 height_;
	Matcher matcher_;
	// lazily evaluated matrix
	// NOTE: need unique span since the iterator type might be
	// non-trivially-destructible (e.g. the case for stdc++ debug mode)
	UniqueSpan<EvalMatch> matchMatrix_;
	float bestMatch_ {-1.f};
	std::pair<u32, u32> bestRes_ {};
	float branchThreshold_;

	// debug functionality
	u32 numEvals_ {};
	u32 numSteps_ {};

	QSet candidates_;
};

float maxPossibleScore(float score, u32 width, u32 height, u32 i, u32 j);

} // namespace vil
