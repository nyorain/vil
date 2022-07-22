#include <lmm.hpp>
#include <util/list.hpp>
#include <util/profiling.hpp>

// PERF: figure out bottlenecks.
// Maybe? inserting into the list. Replace the linked list
// with a heap. Can't use linked list anymore then, re-allocate
// span<Candidate> as needed?
//
// OTOH, we shouldn't really need a heap, inserting can't take that
// long since new candidates will usually be near the top of the
// queue (since they have a score >= the top one, metric can be
// 1 less at max), so inserting should be O(n), but often more
// close to O(1)?
// Alternatively, could store a vector of the last couple of
// insertions and start with the closest one

namespace vil {

float maxPossibleScore(float score, u32 width, u32 height, u32 i, u32 j) {
	return score + std::min(width - i, height - j);
}

// LazyMatrixMarch
LazyMatrixMarch::LazyMatrixMarch(u32 width, u32 height, LinAllocator& alloc,
	Matcher matcher, float branchThreshold) :
		alloc_(alloc), width_(width), height_(height), matcher_(std::move(matcher)),
		branchThreshold_(branchThreshold), set_(HeapCandCompare{*this}, alloc) {

	dlg_assert(width > 0);
	dlg_assert(height > 0);
	dlg_assert(matcher_);

	matchMatrix_ = alloc.alloc<EvalMatch>(width * height);

	// init queue and freeList
	queue_.next = &queue_;
	queue_.prev = &queue_;
	freeList_.next = &freeList_;
	freeList_.prev = &freeList_;

	// add initial candidate
	// 1
	//  insertCandidate(0, 0, 0.f);
	// /*
	// if(height > width) {
	// 	for(auto i = 1u; i < height - width; ++i) {
	// 		insertCandidate(0, i, 0.f);
	// 	}
	// } else {
	// 	for(auto i = 1u; i < width - height; ++i) {
	// 		insertCandidate(i, 0, 0.f);
	// 	}
	// }
	// */

	// 2
	// cands_ = alloc.alloc<HeapCand>(width * height);
	// cands_[0] = {0, 0, 0.f};
	// candCount_ = 1u;

	// 3
	auto it = set_.insert({0, 0, 0.f}).first;
	match(0, 0).best = 0.f;
	match(0, 0).hasCandidate = true;
	match(0, 0).candidate = it;
}

void LazyMatrixMarch::addCandidate(float score, u32 i, u32 j, u32 addI, u32 addJ) {
	ExtZoneScoped;

	if(i + addI >= width() || j + addJ >= height()) {
		// we have a finished run.
		if(score > bestMatch_) {
			bestMatch_ = score;
			dlg_assert(i < width());
			dlg_assert(j < height());
			bestRes_ = {i, j};
		}

		return;
	}

	auto maxPossible = maxPossibleScore(score, i + addI, j + addJ);
	if(maxPossible > bestMatch_) {
		// TODO: retrieving this here kinda costly and redundant to the
		// check in step(). But it's an early out that cuts down
		// the number of steps/candidates a lot so probably worth doing
		// (we otherwise early-out in step() often).
		auto& m = match(i + addI, j + addJ);
		if(m.best < score) {
			// TODO: can be made more efficient, don't insert from 0
			// if(m.candidate) {
			// 	if(m.candidate->score >= score) {
			// 		return;
			// 	}
			// 	unlink(*m.candidate);
			// 	insertBefore(freeList_, *m.candidate);
			// }

			// m.candidate = &insertCandidate(i + addI, j + addJ, score);

			// 2
			// cands_[candCount_] = {u16(i + addI), u16(j + addJ), score};
			// ++candCount_;
			// std::push_heap(cands_.begin(), cands_.begin() + candCount_,
			// 	[&](const auto& a, const auto& b) {
			// 		return maxPossibleScore(a.score, a.i, a.j) + 0.000001 * a.score <
			// 			maxPossibleScore(b.score, b.i, b.j) + 0.000001 * b.score;
			// });

			// 3
			if(m.hasCandidate) {
				// auto c = set_.erase({u16(i + addI), u16(j + addJ), m.best});
				// dlg_assert(c == 1u);
				set_.erase(m.candidate);
			}

			auto [it, succ] = set_.insert({u16(i + addI), u16(j + addJ), score});
			dlg_assert(succ);
			m.hasCandidate = true;
			m.candidate = it;
			m.best = score;
		}
	}
}

bool LazyMatrixMarch::step() {
	ExtZoneScoped;

	if(empty()) {
		return false;
	}

	++numSteps_;

	// 1
	// const auto cand = popCandidate();

	// 2
	// std::pop_heap(cands_.begin(), cands_.begin() + candCount_,
	// 	[&](const auto& a, const auto& b) {
	// 		return maxPossibleScore(a.score, a.i, a.j) + 0.000001 * a.score <
	// 			maxPossibleScore(b.score, b.i, b.j) + 0.000001 * b.score;
	// });
	// --candCount_;
	// const auto cand = cands_[candCount_];

	// 3
	auto it = set_.end();
	--it;
	auto cand = *it;
	set_.erase(it); // pop_back, basically

	// should be true due to pruning
	// (i guess can be false when our metric does not fulfill the
	// assumption used in prune(newScore) about the ordering)
	// NOTE: we need the weird cand.score extra term due to the metric chosen,
	// pruning might not always catch everything
	// dlg_assert_or(maxPossibleScore(cand.score, cand.i, cand.j) + 0.00001 * cand.score >= bestMatch_,
	// 	return true);
	// NOTE can happen in heap, without pruning
	// if(maxPossibleScore(cand.score, cand.i, cand.j) <= bestMatch_) {
	// 	return true;
	// }
	dlg_assert(maxPossibleScore(cand.score, cand.i, cand.j) >= bestMatch_);

	auto& m = this->match(cand.i, cand.j);
	// m.candidate = nullptr;
	m.hasCandidate = false;

	dlg_assert(m.best == cand.score);
	// if(m.best >= cand.score) {
	// 	return true;
	// }

	m.best = cand.score;
	if(m.eval == -1.f) {
		ExtZoneScopedN("eval");
		m.eval = matcher_(cand.i, cand.j);
		++numEvals_;
	}

	if(m.eval > 0.f) {
		auto newScore = cand.score + m.eval;
		addCandidate(newScore, cand.i, cand.j, 1, 1);

		// throw out all candidates that can't even reach what we have
		prune(newScore);
	}

	// TODO: yeah with fuzzy matching we should always branch
	// out... This will generate so many candidates tho :(
	// otoh they have a lower score so won't be considered.
	// And for perfect matches we still only generate 3 * n
	// candidates total.
	// TODO: only threshold = 1.f is guaranteed to be 100% correct,
	// otherwise it's a heuristic.
	if(m.eval < branchThreshold_) {
		addCandidate(cand.score, cand.i, cand.j, 1, 0);
		addCandidate(cand.score, cand.i, cand.j, 0, 1);
	}

	return true;
}

LazyMatrixMarch::Result LazyMatrixMarch::run() {
	ExtZoneScoped;

	// run algorithm
	while(step()) /*noop*/;

	// gather results
	Result res;
	auto maxMatches = std::min(width(), height());
	res.matches = alloc_.alloc<ResultMatch>(maxMatches);
	res.totalMatch = bestMatch_;
	auto outID = maxMatches;

	dlg_assert(bestMatch_ >= 0.f);

	auto [i, j] = bestRes_;
	auto& lastMatch = match(i, j);
	dlg_assert(bestMatch_ >= lastMatch.best);
	dlg_assert(bestMatch_ - lastMatch.best <= 1.f);
	if(lastMatch.eval > 0.f) {
		res.matches[outID - 1] = {i, j, lastMatch.eval};
		--outID;
	}

	while(i > 0 && j > 0) {
		auto& score = match(i, j);
		auto& up = match(i, j - 1);
		if(up.best == score.best) {
			--j;
			continue;
		}

		auto& left = match(i - 1, j);
		if(left.best == score.best) {
			--i;
			continue;
		}

		auto& diag = match(i - 1, j - 1);
		dlg_assert(diag.best < score.best);
		dlg_assertm(diag.eval > 0.f && diag.eval <= 1.f, "{}", diag.eval);
		dlg_assertm(std::abs(diag.eval - (score.best - diag.best)) < 0.001,
			"diag.eval: {}, score.best: {}, diag.best: {}",
			diag.eval, score.best, diag.best);

		--i;
		--j;

		dlg_assert(outID != 0);
		res.matches[outID - 1] = {i, j, diag.eval};
		--outID;
	}

	res.matches = res.matches.last(maxMatches - outID);
	return res;
}

float LazyMatrixMarch::maxPossibleScore(float score, u32 i, u32 j) const {
	return vil::maxPossibleScore(score, width_, height_, i, j);
}

LazyMatrixMarch::Candidate& LazyMatrixMarch::insertCandidate(u32 i, u32 j, float score) {
	ExtZoneScoped;

	Candidate* cand;
	if(freeList_.next != &freeList_) {
		cand = freeList_.next;
		unlink(*cand);
	} else {
		cand = &alloc_.construct<Candidate>();
	}

	cand->i = i;
	cand->j = j;
	cand->score = score;

	auto it = queue_.next;
	while(it != &queue_ && metric(*it) > metric(*cand)) {
		it = it->next;
	}

	insertBefore(*it, *cand);
	return *cand;
}

LazyMatrixMarch::Candidate LazyMatrixMarch::popCandidate() {
	dlg_assert(!empty());
	auto ret = *queue_.next;

	auto& newFree = *queue_.next;
	unlink(newFree);
	insertAfter(freeList_, newFree);

	return ret;
}

LazyMatrixMarch::Candidate LazyMatrixMarch::peekCandidate() const {
	dlg_assert(!empty());
	return *queue_.next;
}

void LazyMatrixMarch::prune(float minScore) {
	ExtZoneScoped;

	// 3
	{
		auto it = set_.begin();
		for(; it != set_.end(); ++it) {
			if(maxPossibleScore(it->score, it->i, it->j) >= minScore) {
				break;
			}

			dlg_assert(match(it->i, it->j).hasCandidate);
			match(it->i, it->j).hasCandidate = false;
		}

		if(it != set_.begin()) {
			set_.erase(set_.begin(), it);
		}

		return;
	}

	// PERF: can be implement more efficiently, unlinking and
	// inserting the whole sub-linked-list
	// TODO: assumed we have maxPossibleScore as metric, not working
	// perfectly with other (e.g. score-based)

// #define SLOW_BUT_METRIC_INDEPENDENT_IMPL
#ifndef SLOW_BUT_METRIC_INDEPENDENT_IMPL
	auto it = queue_.prev;
	while(it != &queue_ && maxPossibleScore(it->score, it->i, it->j) < minScore) {
		auto prev = it->prev;

		auto& newFree = *it;
		unlink(newFree);
		insertAfter(freeList_, newFree);

		it = prev;
	}

	// alternative (but SLOW) implementation without metric assumption
#else
	auto it = queue_.prev;
	while(it != &queue_ /*&& maxPossibleScore(it->score, it->i, it->j) < minScore*/) {
		auto prev = it->prev;

		if(maxPossibleScore(it->score, it->i, it->j) < minScore) {
			auto& newFree = *it;
			unlink(*it);
			insertAfter(freeList_, newFree);
		}

		// minimum ordering assumption
		if(it->score >= minScore) {
			break;
		}

		it = prev;
	}
#endif // 0
}

float LazyMatrixMarch::metric(const Candidate& c) const {
	// The '+ 0.01 * ' parts are basically tie-breakers.

	// Metric: prefer high score.
	// This basically means we go depth-first
	// Pro: we have a path through the matrix very quickly, allowing
	//   us to prune/not consider some bad cases
	// Contra: often we will run many complete paths through the matrix,
	//   looks like candidate bubbles. So a lot of iteration needed
	//   in the end to be sure we have the best path.
	//   NOTE: evaluation on random weights showed an explosion of needed
	//   iterations, order of magnitude higher than using the maxPossibleScore
	//   metric, without a lot of improvement otherwise.
	// return c.score + 0.01 * maxPossibleScore(c.score, c.i, c.j);

	// Metric: prefer high possible score.
	// This is more like breadth-first.
	// Pro: allows efficient pruning (see prune). Also results in a lower
	//   number of total iterations
	// Contra: we might evaluate candidates we could have excluded
	//   otherwise
	return maxPossibleScore(c.score, c.i, c.j) + 0.000001 * c.score;

	// Mixed metric. Still showed the problems of the score metric.
	// return maxPossibleScore(c.score, c.i, c.j) + c.score;
}

} // namespace vil
