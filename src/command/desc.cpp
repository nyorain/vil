#include <command/desc.hpp>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <cb.hpp>
#include <swapchain.hpp>
#include <pipe.hpp>
#include <rp.hpp>
#include <threadContext.hpp>
#include <util/util.hpp>
#include <vk/enumString.hpp>
#include <util/profiling.hpp>

namespace vil {

// TODO: to be removed, together with Command::relID in future.
// This whole concept is hacky and we should not rely on it while matching.
void annotateRelIDLegacy(Command* cmd) {
	ThreadMemScope tms;

	using Pair = std::pair<const std::string_view, u32>;
	std::unordered_map<std::string_view, u32,
		std::hash<std::string_view>,
		std::equal_to<std::string_view>,
		LinearScopedAllocator<Pair>> ids {tms};

	while(cmd) {
		if(auto children = cmd->children()) {
			annotateRelIDLegacy(children);

		}

		auto it = ids.find(cmd->nameDesc());
		if(it == ids.end()) {
			ids.emplace(cmd->nameDesc(), 0u);
			cmd->relID = 0u;
		} else {
			cmd->relID = ++it->second;
		}

		cmd = cmd->next;
	}
}

#ifdef VIL_COMMAND_CALLSTACKS
// TODO: consider offset? depends on command type i guess...
bool same(const backward::StackTrace& a, const backward::StackTrace& b,
		unsigned offset = 1u) {
	if(a.size() != b.size()) {
		return false;
	}

	for(auto i = offset; i < a.size(); ++i) {
		if(a[i].addr != b[i].addr) {
			return false;
		}
	}

	return true;
}
#endif // VIL_COMMAND_CALLSTACKS

FindResult find(const Command* root, span<const Command*> dst,
		const CommandDescriptorSnapshot& dstDsState, float threshold) {
	ZoneScoped;

	dlg_assert_or(!dst.empty(), return {});
	dlg_assert(root);

	std::vector<const Command*> bestCmds;
	float bestMatch = threshold;

	for(auto it = root; it; it = it->next) {
		auto m = it->match(*dst[0]);
		auto em = eval(m);

		// NOTE: early-continue on less-than-best here incorrect for
		// non-parent commands we might increase the matching value below.
		if(em == 0.f || (em < bestMatch && dst.size() > 1)) {
			continue;
		}

#ifdef VIL_COMMAND_CALLSTACKS
		// NOTE WIP: use callstacks for matching.
		// Fast reject if they aren't the same?
		// Should probably make this an option, there might be
		// special cases I'm not thinkin of rn.
		if(it->stackTrace && dst[0]->stackTrace &&
				!same(*it->stackTrace, *dst[0]->stackTrace)) {
			continue;
		}
#endif // VIL_COMMAND_CALLSTACKS

		std::vector<const Command*> currCmds {it};

		if(dst.size() > 1) {
			dlg_assert(it->children());
			auto newThresh = bestMatch / em;
			auto restResult = find(it->children(), dst.subspan(1), dstDsState, newThresh);
			if(restResult.hierachy.empty()) {
				continue;
			}

			auto& rest = restResult.hierachy;
			currCmds.insert(currCmds.end(), rest.begin(), rest.end());
			em *= restResult.match;
		} else if(auto srcCmd = dynamic_cast<const StateCmdBase*>(it); srcCmd) {
			// match descriptors, if any
			// TODO: only consider descriptors statically used by pipeline
			// NOTE: the `min(dsCount, ...)` is used defensively here to
			//   account for extensions that mess with bound-descriptor
			//   requirements, e.g. push descriptors.
			auto* dstCmd = dynamic_cast<const StateCmdBase*>(dst[0]);
			dlg_assert_or(dstCmd, continue);

			auto* srcPipe = srcCmd->boundPipe();
			auto* dstPipe = dstCmd->boundPipe();
			dlg_assert_or(srcPipe && dstPipe, continue);
			dlg_assert_or(srcPipe == dstPipe, continue);

			auto srcDescriptors = srcCmd->boundDescriptors().descriptorSets;
			auto dstDescriptors = dstCmd->boundDescriptors().descriptorSets;

			auto dsCount = dstPipe->layout->descriptors.size();
			auto srcBound = srcDescriptors.first(std::min(dsCount, srcDescriptors.size()));
			auto dstBound = dstDescriptors.first(std::min(dsCount, dstDescriptors.size()));

			if(!dstBound.empty() || !srcBound.empty()) {
				// TODO: consider dynamic offsets?
				for(auto i = 0u; i < std::min(srcBound.size(), dstBound.size()); ++i) {
					if(!srcBound[i].dsEntry || !dstBound[i].dsEntry) {
						// TODO: not sure if this can happen. Do sets
						// that are statically not used by pipeline
						// have to be bound?
						dlg_warn("ds not bound? shouldn't happen");
						continue;
					}

					// We can safely directly access the BoundDescriptorSet
					// here since we can assume the given record to be valid.
					auto& srcDs = access(srcBound[i]);
					auto dstDsCow = dstDsState.states.find(dstBound[i].dsEntry);
					// TODO: we might not find it here due to the new
					// descriptor set capturing rework.
					if(dstDsCow == dstDsState.states.end()) {
						continue;
					}

					auto [dstDs, lock] = access(*dstDsCow->second);

					auto res = vil::match(srcDs, dstDs);
					m.match += res.match;
					m.total += res.total;
				}

				// auto dsMatch = float(match) / std::max(srcBound.size(), dstBound.size());
				// dlg_trace("match: {}, dsMatch: {}", m, dsMatch);
			}
		}

		em = eval(m);
		if(em == 0.f || em < bestMatch) {
			continue;
		} else if(em == bestMatch && !bestCmds.empty()) {
			// when the match values of two commands are equal, choose
			// simply by order in current hierachy level.
			// XXX: maybe it's worth it to always consider this?
			//   We already consider this in Command::match but not the
			//   custom implementations
			if(std::abs(int(currCmds.front()->relID) - int(dst.front()->relID)) >=
					std::abs(int(bestCmds.front()->relID) - int(dst.front()->relID))) {
				continue;
			}
		}

		bestCmds.clear();
		bestCmds = std::move(currCmds);
		bestMatch = em;
	}

	return {bestCmds, bestMatch};
}

// Basically just LCS implementations, see
// https://en.wikipedia.org/wiki/Longest_common_subsequence_problem
// TODO: can probably use a better algorithm but haven't found one
//   yet. We are full O(n^2) here. For everything.
//   But since n is the number of total ParentCommands per frame
//   (pretty much only labels or render passes) this should be fine.
//   Note that the recursive nature of our matching DOES help, i.e.
//   deep nests of ParentCommands (e.g. via lables) are easier to match
//   due to early-outs when matching on high levels.
// TODO: at least early-skip common begin/end. Which means this would
//   be ~O(n) for very similar batches which would be significantly better.
//   This would also make our recursive matching even more helpful for performance!
// TODO: I really feel like we could come up with some lazy-matching algorithm
//   that tries the diagonal first and then expands from there. Pretty sure
//   that we can be SURE at some points that no way through the matrix
//   further away from the diagonal will be faster. Effectively giving
//   us something closer to O(n) again
// TODO: memory optimization: our current use of ThreadMemScope will
//   keep all memory needed during matching alive. That's bad.
//   Could recursively more local ThreadMemScopes. But then we'd have
//   to copy the selected matches on return :(
//   Maybe don't do it recursively but only once per match meta-level.
//   Copy overhead should be acceptable and it's way less of a memory issue.

void add(Matcher& m, const ParentCommand::SectionStats& a, const ParentCommand::SectionStats& b) {
	auto addMatch = [&](u32 dst, u32 src, float weight = 1.f) {
		m.match += weight * std::min(dst, src); // in range [0, max]
		m.total += weight * std::max(dst, src);
	};

	addMatch(a.numDispatches, b.numDispatches);
	addMatch(a.numDraws, b.numDraws);
	addMatch(a.numRayTraces, b.numRayTraces);
	addMatch(a.numTransfers, b.numTransfers);
	addMatch(a.numSyncCommands, b.numSyncCommands);

	addMatch(a.numTotalCommands, b.numTotalCommands);

	// every pipeline match counts like N commands
	const auto pipeWeight = 10.f;
	m.total += pipeWeight * std::max(a.numPipeBinds, b.numPipeBinds);

	// TODO: slightly asymmetrical in special cases. Problem?
	for(auto pipeA = b.boundPipelines; pipeA; pipeA = pipeA->next) {
		for(auto pipeB = b.boundPipelines; pipeB; pipeB = pipeB->next) {
			if(pipeA->pipe == pipeB->pipe) {
				m.match += pipeWeight;
				break;
			}
		}
	}
}

std::pair<span<SectionMatch>, Matcher> match(ThreadMemScope& tms,
		const ParentCommand& rootA, const ParentCommand& rootB) {
	// TODO: fast patch for &rootA == &rootB
	// take additional parameter on whether the matches are really needed?
	// in many cases they are not, which would make this a lot cheaper.

	// match commands themselves
	auto rootMatch = rootA.match(rootB);
	dlg_assert(rootMatch.total == -1.f || rootMatch.match <= rootMatch.total);

	if(rootMatch.match <= 0.f) {
		return {{}, rootMatch};
	}

#ifdef VIL_COMMAND_CALLSTACKS
	// NOTE WIP: use callstacks for matching.
	// Fast reject if they aren't the same?
	// Should probably make this an option, there might be
	// special cases I'm not thinkin of rn.
	if(rootA.stackTrace && rootB.stackTrace &&
			!same(*rootA.stackTrace, *rootB.stackTrace)) {
		return {{}, Matcher::noMatch()};
	}
#endif // VIL_COMMAND_CALLSTACKS

	// consider sectionStats for rootMatch.
	auto statsA = rootA.sectionStats();
	auto statsB = rootB.sectionStats();
	add(rootMatch, statsA, statsB);
	dlg_assert(rootMatch.total == -1.f || rootMatch.match <= rootMatch.total);

	// setup match matrix
	struct Entry {
		Matcher match {}; // accumulated
		unsigned dir {};
		float matchHere {};
		const ParentCommand* cA {};
		const ParentCommand* cB {};
		span<SectionMatch> children;
	};

	auto numSectionsA = rootA.sectionStats().numChildSections;
	auto numSectionsB = rootB.sectionStats().numChildSections;

	auto maxSections = std::max(numSectionsA, numSectionsB);
	auto minSections = std::min(numSectionsA, numSectionsB);

	if(maxSections == 0u) {
		return {{}, rootMatch};
	}

	// one of them has no sections, the other one has.
	if(minSections == 0u) {
		auto missedMin = 10;
		rootMatch.total += missedMin * maxSections;
		return {{}, rootMatch};
	}

	auto entries = tms.alloc<Entry>(numSectionsA * numSectionsB);
	auto entry = [&](auto ia, auto ib) -> decltype(auto) {
		dlg_assert(ia < numSectionsA);
		dlg_assert(ib < numSectionsB);
		return entries[ia * numSectionsB + ib];
	};

	// fill matrix
	auto itA = rootA.firstChildParent();
	auto ia = 0u;
	while(itA) {
		auto itB = rootB.firstChildParent();
		auto ib = 0u;
		while (itB) {
			Matcher valDiag = (ia == 0u || ib == 0u) ? Matcher {} : entry(ia - 1, ib - 1).match;
			auto [childMatches, matchVal] = match(tms, *itA, *itB);
			if(matchVal.match > 0) {
				dlg_assert(matchVal.match <= matchVal.total);
				valDiag.match += matchVal.match;
				valDiag.total += matchVal.total;
			}

			auto valUp = (ia == 0u) ? Matcher{} : entry(ia - 1, ib).match;
			auto valLeft = (ib == 0u) ? Matcher{} : entry(ia, ib - 1).match;

			// NOTE: instead of eval() we only consider the match values
			// here, not the total values. This means, we allow (even huge)
			// mismatches to just straight up maximize the number of matches
			// we have.
			// TODO: might cause issues, might need finetuning. investigate
			const auto evalUp = valUp.match;
			const auto evalLeft = valLeft.match;
			const auto evalDiag = matchVal.match > 0 ? valDiag.match : -1.f;
			const auto maxEval = std::max(evalDiag, std::max(evalUp, evalLeft));

			auto& dst = entry(ia, ib);
			dst.matchHere = eval(matchVal);
			dst.children = childMatches;
			dst.cA = itA;
			dst.cB = itB;
			if(maxEval == evalDiag) {
				dst.dir = 1u;
				dst.match = valDiag;
			} else if(maxEval == evalUp) {
				dst.dir = 2u;
				dst.match = valUp;
			} else if(maxEval == evalLeft) {
				dst.dir = 3u;
				dst.match = valLeft;
			} else {
				dlg_fatal("unreachable: {} = max({}, {}, {})",
					maxEval, evalDiag, evalUp, evalLeft);
			}

			itB = itB->nextParent_;
			++ib;
		}

		dlg_assert(ib == numSectionsB);

		itA = itA->nextParent_;
		++ia;
	}

	dlg_assert(ia == numSectionsA);

	// backtrack
	auto maxNumMatches = minSections;
	span<SectionMatch> resMatches = tms.alloc<SectionMatch>(maxNumMatches);

	ia = numSectionsA;
	auto ib = numSectionsB;
	auto outID = 0u;
	while(ia != 0u && ib != 0u) {
		auto& src = entry(ia - 1, ib - 1);
		if(src.dir == 1u) {
			auto& sm = resMatches[outID++];
			sm.a = src.cA;
			sm.b = src.cB;
			sm.match = src.matchHere;
			sm.children = src.children;

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

	resMatches = resMatches.first(outID);
	auto numMatchedSections = outID;
	dlg_assert(numMatchedSections <= minSections);

	auto& bestPath = entries.back().match;
	rootMatch.total += bestPath.total;
	rootMatch.match += bestPath.match;
	dlg_assert(rootMatch.total == -1.f || rootMatch.match <= rootMatch.total);

	// we assume that the total match lost by missing sections is the same
	// as the match, but at least 1.
	auto diff = (maxSections - numMatchedSections);
	auto missedMin = 10;
	if(numMatchedSections == 0u || bestPath.total < missedMin * diff) {
		rootMatch.total += missedMin * diff;
	} else {
		rootMatch.total += diff * bestPath.total / numMatchedSections;
	}

	return {resMatches, rootMatch};
}

BatchMatch match(ThreadMemScope& tms, const FrameSubmission& a, const FrameSubmission& b) {
	ZoneScoped;

	if(a.queue != b.queue) {
		return {0.f, &a, &b, {}};
	}

	if(a.submissions.size() == 0u && b.submissions.size() == 0u) {
		return {1.f, &a, &b, {}};
	}

	// TODO: handle this case, causes memory error atm.
	dlg_assert(a.submissions.size() > 0 || b.submissions.size() > 0);

	struct Entry {
		float match {};
		unsigned dir {};
		float matchHere {};
		span<SectionMatch> matches;
	};

	auto entries = tms.alloc<Entry>(a.submissions.size() * b.submissions.size());
	auto entry = [&](auto ia, auto ib) -> decltype(auto) {
		return entries[ia * b.submissions.size() + ib];
	};

	for(auto ia = 0u; ia < a.submissions.size(); ++ia) {
		for(auto ib = 0u; ib < b.submissions.size(); ++ib) {
			// TODO: consider additional information about the record instead
			// of just the commands? e.g. usage flags, callstacks of
			// beginRecord/endRecord?
			auto [matches, matcher] = match(tms,
				*a.submissions[ia]->commands,
				*b.submissions[ib]->commands);
			// TODO: take matcher.total into account
			auto matchFac = eval(matcher);
			auto valDiag = -1.f;
			if(matchFac > 0.0) {
				valDiag = (ia == 0u || ib == 0u) ? 0.f : entry(ia - 1, ib - 1).match;
				valDiag += matchFac;
			}

			auto valUp = (ia == 0u) ? 0.f : entry(ia - 1, ib).match;
			auto valLeft = (ib == 0u) ? 0.f : entry(ia, ib - 1).match;

			auto& dst = entry(ia, ib);
			dst.match = std::max(valDiag, std::max(valUp, valLeft));
			dst.matchHere = matchFac;
			dst.matches = matches;
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
	BatchMatch res;
	res.a = &a;
	res.b = &b;
	if(entries.empty()) {
		res.match = 0.f;
		return res;
	}

	res.match = entries.back().match;
	res.matches = tms.alloc<RecordMatch>(std::min(a.submissions.size(), b.submissions.size()));

	auto ia = a.submissions.size();
	auto ib = b.submissions.size();
	auto outID = 0u;
	while(ia != 0u && ib != 0u) {
		auto& src = entry(ia - 1, ib - 1);
		if(src.dir == 1u) {
			auto& bm = res.matches[outID++];
			bm.a = a.submissions[ia - 1].get();
			bm.b = b.submissions[ib - 1].get();
			bm.match = src.matchHere;
			bm.matches = src.matches;

			--ia;
			--ib;
		} else if(src.dir == 2u) {
			--ia;
		} else if(src.dir == 3u) {
			--ib;
		} else {
			dlg_fatal("unreachable: src.dir = {}", src.dir);
		}
	}

	res.matches = res.matches.first(outID);
	return res;
}

MatchResult match(ThreadMemScope& tms, span<const FrameSubmission> a, span<const FrameSubmission> b) {
	ZoneScoped;

	if(a.empty() || b.empty()) {
		// TODO: for a.empty && b.empty return match = 1.f? relevant anywhere?
		return {};
	}

	struct Entry {
		float match {};
		unsigned dir {};
		BatchMatch matches;
	};

	auto entries = tms.alloc<Entry>(a.size() * b.size());
	auto entry = [&](auto ia, auto ib) -> decltype(auto) {
		return entries[ia * b.size() + ib];
	};

	for(auto ia = 0u; ia < a.size(); ++ia) {
		for(auto ib = 0u; ib < b.size(); ++ib) {
			auto valDiag = -1.f;
			// TODO: take matcher.total into account
			auto matchRes = match(tms, a[ia], b[ib]);
			if(matchRes.match > 0.f) {
				valDiag = ((ia == 0u || ib == 0u) ? 0.f : entry(ia - 1, ib - 1).match);
				valDiag += matchRes.match;
			}

			auto valUp = (ia == 0u) ? 0.f : entry(ia - 1, ib).match;
			auto valLeft = (ib == 0u) ? 0.f : entry(ia, ib - 1).match;

			auto& dst = entry(ia, ib);
			dst.match = std::max(valDiag, std::max(valUp, valLeft));
			dst.matches = std::move(matchRes);
			if(dst.match == valDiag) {
				dst.dir = 1u;
			} else if(dst.match == valUp) {
				dst.dir = 2u;
			} else if(dst.match == valLeft) {
				dst.dir = 3u;
			} else {
				dlg_fatal("unreachable");
			}
		}
	}

	// backtrack
	MatchResult res;
	res.matches = tms.alloc<BatchMatch>(std::min(a.size(), b.size()));
	res.match = entries.back().match;

	auto ia = a.size();
	auto ib = b.size();
	auto outID = 0u;
	while(ia != 0u && ib != 0u) {
		auto& src = entry(ia - 1, ib - 1);
		if(src.dir == 1u) {
			auto& bm = res.matches[outID++];
			bm = std::move(src.matches);

			--ia;
			--ib;
		} else if(src.dir == 2u) {
			--ia;
		} else if(src.dir == 3u) {
			--ib;
		} else {
			dlg_fatal("unreachable: src.dir = {}", src.dir);
		}
	}

	res.matches = res.matches.first(outID);
	return res;
}

} // namespace vil
