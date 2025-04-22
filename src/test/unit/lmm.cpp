#include "../bugged.hpp"
#include "../approx.hpp"
#include "lmm.hpp"
#include <random>
#include <chrono>
#include <unordered_set>
#include <util/profiling.hpp>

using namespace vil;

namespace vil::bugged {

template<typename T>
struct Printable<Approx<T>> {
	static std::string call(const Approx<T>& approx) {
		static auto txt = std::string("Approx(") + std::to_string(approx.value) + ")";
		return txt;
	}
};

}

// Copied ferom the old command/desc.cpp implementation
struct SlowAlignAlgo {
	using Matcher = std::function<float(u32 i, u32 j)>;

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

	static Result run(LinAllocator& alloc, u32 lenA, u32 lenB, Matcher matcher) {
		ExtZoneScoped;

		struct Entry {
			float match {}; // accumulated
			unsigned dir {};
			float matchHere {};
		};

		auto entries = alloc.alloc<Entry>(lenA * lenB);
		auto entry = [&](auto ia, auto ib) -> decltype(auto) {
			dlg_assert(ia < lenA);
			dlg_assert(ib < lenB);
			return entries[ia * lenB + ib];
		};

		// fill matrix
		for(auto ia = 0u; ia < lenA; ++ia) {
			for(auto ib = 0u; ib < lenB; ++ib) {
				ExtZoneScopedN("iteration");
				float matchVal;

				{
					ExtZoneScopedN("eval");
					matchVal = matcher(ia, ib);
				}

				auto valDiag = -1.f;
				if(matchVal > 0.0) {
					valDiag = (ia == 0u || ib == 0u) ? 0.f : entry(ia - 1, ib - 1).match;
					valDiag += matchVal;
				}

				auto valUp = (ia == 0u) ? 0.f : entry(ia - 1, ib).match;
				auto valLeft = (ib == 0u) ? 0.f : entry(ia, ib - 1).match;

				auto& dst = entry(ia, ib);
				dst.matchHere = matchVal;
				dst.match = std::max(valDiag, std::max(valUp, valLeft));
				if(dst.match == valDiag) {
					dst.dir = 1u;
				} else if(dst.match == valUp) {
					dst.dir = 2u;
				} else if(dst.match == valLeft) {
					dst.dir = 3u;
				} else {
					dlg_fatal("unreachable: {} = max({}, {}, {})",
						dst.match, valDiag, valUp, valLeft);
				}
			}
		}

		// backtrack
		auto maxNumMatches = std::min(lenA, lenB);
		Result res;
		res.matches = alloc.alloc<ResultMatch>(maxNumMatches);
		res.totalMatch = entries.back().match;
		auto outID = maxNumMatches;

		auto ia = lenA;
		auto ib = lenB;

		// if(entries.back().matchHere > 0.f) {
		// 	res.matches[outID - 1].matchVal = entries.back().matchHere;
		// 	res.matches[outID - 1].i = ia - 1;
		// 	res.matches[outID - 1].j = ib - 1;
		// 	--outID;
		// }

		while(ia != 0u && ib != 0u) {
			auto& src = entry(ia - 1, ib - 1);
			if(src.dir == 1u) {
				dlg_assert(outID > 0);
				auto& sm = res.matches[outID - 1];
				sm.i = ia - 1;
				sm.j = ib - 1;
				sm.matchVal = src.matchHere;

				--outID;
				--ia;
				--ib;
			} else if(src.dir == 2u) {
				--ia;
			} else if(src.dir == 3u) {
				--ib;
			} else {
				dlg_fatal("unreachable: dir = {}", src.dir);
			}
		}

		res.matches = res.matches.last(maxNumMatches - outID);
		return res;
	}
};

TEST(unit_identity_square) {
	auto matcher = [](u32 i, u32 j) -> float {
		return i == j ? 1.f : 0.f;
	};

	LinAllocator alloc;
	LazyMatrixMarch lmm(32u, 32u, alloc, matcher);

	auto res = lmm.run();
	EXPECT(res.totalMatch, approx(32 * 1.f));
	EXPECT(res.matches.size(), 32u);

	for(auto i = 0u; i < res.matches.size(); ++i) {
		EXPECT(res.matches[i].matchVal, 1.f);
		EXPECT(res.matches[i].i, i);
		EXPECT(res.matches[i].j, i);
	}
}

TEST(unit_identity_imperfect_rect) {
	constexpr auto matchVal = 0.4f;
	auto matcher = [=](u32 i, u32 j) -> float {
		return i == j ? matchVal : 0.f;
	};

	LinAllocator alloc;
	LazyMatrixMarch lmm(17u, 64u, alloc, matcher);

	auto res = lmm.run();
	EXPECT(res.totalMatch, approx(17.f * 0.4f, 0.001));
	EXPECT(res.matches.size(), 17u);

	for(auto i = 0u; i < res.matches.size(); ++i) {
		EXPECT(res.matches[i].matchVal, matchVal);
		EXPECT(res.matches[i].i, i);
		EXPECT(res.matches[i].j, i);
	}
}

// just for safety, unit test testing our trivial implementation
TEST(unit_validate_trivial) {
	constexpr auto matchVal = 0.4f;
	auto matcher = [=](u32 i, u32 j) -> float {
		return i == j ? matchVal : 0.f;
	};

	LinAllocator alloc;
	auto res = SlowAlignAlgo::run(alloc, 17u, 64u, matcher);
	EXPECT(res.totalMatch, approx(17.f * 0.4f, 0.001));
	EXPECT(res.matches.size(), 17u);

	for(auto i = 0u; i < res.matches.size(); ++i) {
		EXPECT(res.matches[i].matchVal, matchVal);
		EXPECT(res.matches[i].i, i);
		EXPECT(res.matches[i].j, i);
		// dlg_info("{} - {}: {}", res.matches[i].i, res.matches[i].j, res.matches[i].matchVal);
	}
}

TEST(unit_compare_trivial) {
	auto lenA = 31u;
	auto lenB = 44u;
	auto weights_ = std::make_unique<double[]>(lenA * lenB);

	std::random_device rd;
	std::mt19937 e2(rd());
	std::uniform_real_distribution<> dist(0, 1);
	for(auto y = 0u; y < lenB; ++y) {
		for(auto x = 0u; x < lenA; ++x) {
			// auto weight = dist(e2);

			auto weight = 0.f;
			// float w = std::max(dist(e2), 0.0);
			// w = 1 - w * w * w;
			// if(w > 0.8) { //0.1 * std::abs(int(x) - int(y))) {
			// 	weight = std::max(std::min(w, 1.f), 0.f);
			// }

			// TODO: with this, an expected value on the boundary fails.
			// figure out the correct solution
			if(x == y) {
				weight = 1.f;
			}

			weights_[y * lenA + x] = weight;
		}
	}

	auto matcher = [&](u32 i, u32 j) -> float {
		// simulate some comparison operation
		// std::this_thread::sleep_for(std::chrono::microseconds(5));
		// return i == j ? 1.f : 0.f;

		// float w = 1.f;
		// for(auto i = 1u; i < 256u; ++i) {
		// 	w += std::abs(0.01 * i - 5.0 / j);
		// }

		return weights_[j * lenA + i]; // - 0.0000001 * w;
	};

	using Clock = std::chrono::high_resolution_clock;
	LinAllocator alloc;
	LazyMatrixMarch llm(lenA, lenB, alloc, matcher, 1.f);

	auto before = Clock::now();
	auto resLLM = llm.run();
	auto timeLLM = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - before).count();
	dlg_trace("timeLLM: {} mus", timeLLM);

	before = Clock::now();
	auto resRef = SlowAlignAlgo::run(alloc, lenA, lenB, matcher);
	auto timeRef = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - before).count();
	dlg_trace("timeRef: {} mus", timeRef);

	EXPECT(resLLM.matches.size(), resRef.matches.size());
	EXPECT(resLLM.totalMatch, approx(resRef.totalMatch, 0.001));

	dlg_trace("random weight match: {}", resLLM.totalMatch);
	dlg_trace("lmm comparisons: {}, trivial: {}", llm.numEvals(), lenA * lenB);
	dlg_trace("lmm steps: {}, trivial: {}", llm.numSteps(), lenA * lenB);

	for(auto i = 0u; i < std::min(resLLM.matches.size(), resRef.matches.size()); ++i) {
		auto& mref = resRef.matches[i];
		auto& mllm = resLLM.matches[i];
		EXPECT(mllm.i, mref.i);
		EXPECT(mllm.j, mref.j);
		EXPECT(approx(mllm.matchVal, 0.001), mref.matchVal);
	}
}

TEST(unit_lmm_strings) {
	auto checkMatch = [](std::string_view strA, std::string_view strB, u32 count) {
		auto matcher = [&](u32 i, u32 j) -> float {
			return strA[i] == strB[j] ? 1.f : 0.f;
		};

		LinAllocator alloc;
		LazyMatrixMarch llm(strA.size(), strB.size(), alloc, matcher, 1.f);
		auto res = llm.run();

		std::unordered_set<u32> seenA;
		std::unordered_set<u32> seenB;

		EXPECT(res.matches.size(), count);

		for(auto& m : res.matches) {
			EXPECT(seenA.count(m.i), 0u);
			EXPECT(seenB.count(m.j), 0u);

			EXPECT(strA[m.i], strB[m.j]);
			EXPECT(m.matchVal, 1.f);

			seenA.insert(m.i);
			seenB.insert(m.j);
		}
	};

	checkMatch("abcde", "0abceff", 4);
	checkMatch("abcd", "abcd", 4);
	checkMatch("abcdfff", "abcd", 4);
	checkMatch("abcd", "abcdfff", 4);
	checkMatch("fffabcd", "abcdfff", 4);
	checkMatch("fffabcdxxx", "abcdfff", 4);
	checkMatch("fffabcdxxx", "xxxxabcdfff", 4);
	checkMatch("abcd", "aaaa", 1);
	checkMatch("dddd", "aaaa", 0);
	checkMatch("dddda", "aaaa", 1);
	checkMatch("dddda", "da", 2);
	checkMatch("dddda", "aaaad", 1);
	checkMatch("a", "aaaadaaaa", 1);
}
