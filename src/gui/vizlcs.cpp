#include <gui/vizlcs.hpp>
#include <util/list.hpp>
#include <random>
#include <iomanip>

inline ImVec2 operator*(const ImVec2& lhs, const float rhs)              { return ImVec2(lhs.x * rhs, lhs.y * rhs); }
inline ImVec2 operator/(const ImVec2& lhs, const float rhs)              { return ImVec2(lhs.x / rhs, lhs.y / rhs); }
inline ImVec2 operator+(const ImVec2& lhs, const ImVec2& rhs)            { return ImVec2(lhs.x + rhs.x, lhs.y + rhs.y); }
inline ImVec2 operator-(const ImVec2& lhs, const ImVec2& rhs)            { return ImVec2(lhs.x - rhs.x, lhs.y - rhs.y); }
inline ImVec2 operator*(const ImVec2& lhs, const ImVec2& rhs)            { return ImVec2(lhs.x * rhs.x, lhs.y * rhs.y); }
inline ImVec2 operator/(const ImVec2& lhs, const ImVec2& rhs)            { return ImVec2(lhs.x / rhs.x, lhs.y / rhs.y); }
inline ImVec2& operator*=(ImVec2& lhs, const float rhs)                  { lhs.x *= rhs; lhs.y *= rhs; return lhs; }
inline ImVec2& operator/=(ImVec2& lhs, const float rhs)                  { lhs.x /= rhs; lhs.y /= rhs; return lhs; }
inline ImVec2& operator+=(ImVec2& lhs, const ImVec2& rhs)                { lhs.x += rhs.x; lhs.y += rhs.y; return lhs; }
inline ImVec2& operator-=(ImVec2& lhs, const ImVec2& rhs)                { lhs.x -= rhs.x; lhs.y -= rhs.y; return lhs; }
inline ImVec2& operator*=(ImVec2& lhs, const ImVec2& rhs)                { lhs.x *= rhs.x; lhs.y *= rhs.y; return lhs; }
inline ImVec2& operator/=(ImVec2& lhs, const ImVec2& rhs)                { lhs.x /= rhs.x; lhs.y /= rhs.y; return lhs; }
inline ImVec4 operator+(const ImVec4& lhs, const ImVec4& rhs)            { return ImVec4(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, lhs.w + rhs.w); }
inline ImVec4 operator-(const ImVec4& lhs, const ImVec4& rhs)            { return ImVec4(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z, lhs.w - rhs.w); }
inline ImVec4 operator*(const ImVec4& lhs, const ImVec4& rhs)            { return ImVec4(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z, lhs.w * rhs.w); }

namespace vil {

// util
static constexpr auto width = 15u;
static constexpr auto height = 13u;

struct Candidate {
	Candidate* prev {};
	Candidate* next {};

	u32 i;
	u32 j;
	float score;
};

float maxPossibleScore(float score, u32 i, u32 j) {
	return score + std::min(width - i, height - j);
}

struct CandidateQueue {
	CandidateQueue(LinAllocator& la) : tms(la) {
		initListAnchor(queue);
		initListAnchor(freeList);
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

void CandidateQueue::insert(u32 i, u32 j, float score) {
	Candidate* cand;
	if(freeList.next != &freeList) {
		cand = freeList.next;
		unlink(*cand);
	} else {
		cand = &tms.construct<Candidate>();
	}

	cand->i = i;
	cand->j = j;
	cand->score = score;

	auto it = queue.next;
	while(it != &queue && metric(*it) > metric(*cand)) {
		it = it->next;
	}

	insertBefore(*it, *cand);
}

Candidate CandidateQueue::pop() {
	dlg_assert(!empty());
	auto ret = *queue.next;

	auto& newFree = *queue.next;
	unlink(newFree);
	insertAfter(freeList, newFree);

	return ret;
}

Candidate CandidateQueue::peek() {
	dlg_assert(!empty());
	return *queue.next;
}

void CandidateQueue::prune(float minScore) {
	// PERF: can be implement more efficiently, unlinking and
	// inserting the whole sub-linked-list
	// TODO: assumed we have maxPossibleScore as metric, not working
	// perfectly with other score

	auto it = queue.prev;
	while(it != &queue && maxPossibleScore(it->score, it->i, it->j) < minScore) {
		auto prev = it->prev;

		auto& newFree = *it;
		unlink(newFree);
		insertAfter(freeList, newFree);

		it = prev;
	}

	// TODO, alternative (but SLOW) implementation without metric assumption
#if 0
	auto it = queue.prev;
	while(it != &queue /*&& maxPossibleScore(it->score, it->i, it->j) < minScore*/) {
		auto prev = it->prev;

		if(maxPossibleScore(it->score, it->i, it->j) < minScore) {
			auto& newFree = unlink(*it);
			insertAfter(freeList, newFree);
		}

		// minimum ordering assumption
		if(it->score >= minScore) {
			break;
		}

		it = prev;
	}
#endif // 0
}

float CandidateQueue::metric(const Candidate& c) const {
	// The '+ 0.01 * ' parts are basically tie-breakers.

	// return c.score + 0.01 * maxPossibleScore(c.score, c.i, c.j);
	return maxPossibleScore(c.score, c.i, c.j) + 0.01 * c.score;
	// return maxPossibleScore(c.score, c.i, c.j) + c.score;
	// return c.score;
}

// algo
struct FLCS {
	struct Match {
		float eval {-1.f};
		float best {-1.f};
	};

	std::unique_ptr<double[]> weights_;
	std::unique_ptr<Match[]> matchMatrix_;
	LinAllocator alloc;
	CandidateQueue queue;

	float bestMatch_ {};
	std::pair<u32, u32> bestRes_ {};

	FLCS() : queue(alloc) {
		queue.insert(0u, 0u, 0u);

		auto count = width * height;
		matchMatrix_ = std::make_unique<Match[]>(count);

		// init weights
		// TODO: random
		weights_ = std::make_unique<double[]>(count);

		// all other weights are 0
		// for(auto i = 0u; i < std::min(width, height); ++i) {
		// 	weight(i, i) = 1.0;
		// }

		// all weights random
		std::random_device rd;
		std::mt19937 e2(rd());
		std::uniform_real_distribution<> dist(0, 1);
		for(auto y = 0u; y < height; ++y) {
			for(auto x = 0u; x < width; ++x) {
				weight(x, y) = 0;

				auto w = std::max(dist(e2), 0.0);
				w = 1 - w * w * w;
				if(w > 0.8) {
					weight(x, y) = w;
				}
				// if(x == y) {
				// 	weight(x, y) = w;
				// }
			}
		}
	}

	void addCandidate(float score, u32 i, u32 j, u32 addI, u32 addJ) {
		if(i + addI >= width || j + addJ >= height) {
			// we have a finished run.
			if(score > bestMatch_) {
				bestMatch_ = score;
				bestRes_ = {i, j};
			}
		} else {
			auto maxPossible = maxPossibleScore(score, i + addI, j + addJ);
			if(maxPossible > bestMatch_) {
				queue.insert(i + addI, j + addJ, score);
			}
		}
	}

	void step() {
		if(queue.empty()) {
			return;
		}

		auto cand = queue.pop();

		// should be true due to pruning
		// (i guess can be false when our metric does not fulfill the
		// assumption used in prune(newScore) about the ordering)
		dlg_assert(maxPossibleScore(cand.score, cand.i, cand.j) >= bestMatch_);

		auto& m = match(cand.i, cand.j);
		if(m.best >= cand.score + 1) {
			return;
		}

		if(m.eval == -1.f) {
			m.eval = weight(cand.i, cand.j);
		}

		auto newScore = cand.score + m.eval;
		if(newScore > m.best) {
			m.best = newScore;

			if(m.eval > 0.f) {
				addCandidate(newScore, cand.i, cand.j, 1, 1);

				// throw out all candidates that can't even reach what we have
				queue.prune(newScore);
			}

			// TODO: yeah with fuzzy matching we should always branch
			// out... This will generate so many candidates tho :(
			// otoh they have a lower score so won't be considered.
			// And for perfect matches we still only generate 3 * n
			// candidates total.
			// TODO: only threshold = 1.f is guaranteed to be 100% correct,
			// otherwise it's a heuristic.
			constexpr auto threshold = 0.9f;
			if(m.eval < threshold) {
				addCandidate(cand.score, cand.i, cand.j, 1, 0);
				addCandidate(cand.score, cand.i, cand.j, 0, 1);
			}
		}
	}

	Match& match(u32 i, u32 j) {
		return matchMatrix_[height * i + j];
	}

	double& weight(u32 i, u32 j) {
		return weights_[height * i + j];
	}
};

// viz
void drawTexCentered(ImDrawList* dl, ImVec2 pos, u32 color, const char* text) {
	auto textSize = ImGui::CalcTextSize(text);
	dl->AddText(pos - ImVec2(0.5f, 0.5f) * textSize, color, text);
}

void drawField(ImDrawList* dl, ImVec2 pos, ImVec2 size, u32 color) {
	dl->AddRectFilled(pos, pos + size, color);
}

void drawWeight(ImDrawList* dl, ImVec2 pos, ImVec2 size, double weight) {
	auto tp = pos + ImVec2(0.5f, 0.5f) * size;
	auto textStr = dlg::format("{}{}", std::setprecision(3), weight);
	drawTexCentered(dl, tp, 0xFF000000, textStr.c_str());
}

void VizLCS::draw() {
	if(!algo_) {
		algo_ = std::make_unique<FLCS>();
	}

	if(ImGui::Button("Step")) {
		algo_->step();
	}

	auto* dl = ImGui::GetWindowDrawList();

	ImVec2 size{40.f, 40.f};
	ImVec2 pad{8.f, 8.f};

	auto spos = ImGui::GetCursorScreenPos();

	// first, draw all fields, basic
	for(auto y = 0u; y < height; ++y) {
		for(auto x = 0u; x < width; ++x) {
			ImVec2 pos = spos + ImVec2(x, y) * (pad + size);
			drawField(dl, pos, size, 0xFFFFFFFFu);
		}
	}

	// now, draw all candidates
	auto it = algo_->queue.queue.next;
	while(it != &algo_->queue.queue) {
		ImVec2 pos = spos + ImVec2(it->i, it->j) * (pad + size);
		drawField(dl, pos, size, 0xFF6666FFu);
		it = it->next;

		drawWeight(dl, pos + ImVec2(0, 15), size, it->score);
	}

	// now, draw best candidate
	{
		auto it = algo_->queue.queue.next;
		if(it != &algo_->queue.queue) {
			ImVec2 pos = spos + ImVec2(it->i, it->j) * (pad + size);
			drawField(dl, pos, size, 0xFFFF66FFu);
		}
	}

	// now, draw best paths on all discovered fields
	for(auto y = 0u; y < height; ++y) {
		for(auto x = 0u; x < width; ++x) {
			if(algo_->match(x, y).eval == -1.f) {
				continue;
			}

			ImVec2 pos = spos + ImVec2(x, y) * (pad + size);
			drawWeight(dl, pos, size, algo_->match(x, y).best);
		}
	}
}

VizLCS::VizLCS() = default;
VizLCS::~VizLCS() = default;

} // namespace vil
