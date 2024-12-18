#include <lmm.hpp>
#include <util/profiling.hpp>

namespace vil {

float maxPossibleScore(float score, u32 width, u32 height, u32 i, u32 j) {
	return score + std::min(width - i, height - j);
}

// LazyMatrixMarch
LazyMatrixMarch::LazyMatrixMarch(u32 width, u32 height, LinAllocator& alloc,
	Matcher matcher, float branchThreshold) :
		alloc_(alloc), width_(width), height_(height), matcher_(std::move(matcher)),
		branchThreshold_(branchThreshold), candidates_(HeapCandCompare{*this}, alloc) {

	dlg_assert(width > 0);
	dlg_assert(height > 0);
	// if we ever exceed this, just use 32-bit integers for HeapCand.
	// Not that big of a loss, 16-bit was done because of performance
	// and memory concerns. Who matches such HUGE sequences?!
	dlg_assert(width < 1024 * 64);
	dlg_assert(height < 1024 * 64);
	dlg_assert(matcher_);

	matchMatrix_ = alloc.allocNonTrivial<EvalMatch>(width * height);

	for(auto& m : matchMatrix_) {
		m.candidate = candidates_.end();
	}

	// insert first candidate
	auto it = candidates_.insert({0, 0, 0.f}).first;
	match(0, 0).best = 0.f;
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
		// NOTE: retrieving this here kinda costly and redundant to the
		// check in step(). But it's an early out that cuts down
		// the number of steps/candidates a lot so probably worth doing
		// (we otherwise early-out in step() often).
		auto& m = match(i + addI, j + addJ);
		if(m.best < score) {
			if(m.candidate != candidates_.end()) {
				candidates_.erase(m.candidate);
			}

			auto [it, succ] = candidates_.insert({u16(i + addI), u16(j + addJ), score});
			dlg_assert(succ);
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
	auto cand = popCandidate();

	// should be true due to pruning
	dlg_assert(maxPossibleScore(cand) >= bestMatch_);

	auto& m = this->match(cand.i, cand.j);
	m.candidate = candidates_.end();

	// this invariant follows from the way we insert new candidates
	// there is always at most one candidate per field
	dlg_assert(m.best == cand.score);

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

	// NOTE: yeah with fuzzy matching we should always branch
	// out... This will generate so many candidates tho :(
	// otoh they have a lower score so won't be considered.
	// And for perfect matches we still only generate 3 * n
	// candidates total.
	// NOTE: only threshold = 1.f is guaranteed to be 100% correct,
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
	dlg_assert(std::abs(bestMatch_ - (lastMatch.best + lastMatch.eval)) < 0.0001);
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

LazyMatrixMarch::HeapCand LazyMatrixMarch::popCandidate() {
	dlg_assert(!empty());

	auto it = candidates_.end();
	--it;
	auto cand = *it;
	candidates_.erase(it); // pop_back, basically
	return cand;
}

LazyMatrixMarch::HeapCand LazyMatrixMarch::peekCandidate() const {
	auto it = candidates_.end();
	--it;
	return *it;
}

void LazyMatrixMarch::prune(float minScore) {
	ExtZoneScoped;

	// for this to work correctly, it's important that maxPossibleScore
	// is always the primary criterion in the heap comparison function

	auto it = candidates_.begin();
	for(; it != candidates_.end(); ++it) {
		if(maxPossibleScore(*it) >= minScore) {
			break;
		}

		auto& m = match(it->i, it->j);
		dlg_assert(m.candidate != candidates_.end());
		m.candidate = candidates_.end();
	}

	if(it != candidates_.begin()) {
		candidates_.erase(candidates_.begin(), it);
	}

	return;
}

} // namespace vil
