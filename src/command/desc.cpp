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

// TODO: the whole CommandBufferDesc code is suboptimal, really.
// See desc2.hpp and ideas in match.md and todo.md

namespace vil {

// TODO: to be removed, together with Command::relID in future.
// This whole concept is hacky and only needed for gui.
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

		cmd->relID = ids[cmd->nameDesc()]++;
		cmd = cmd->next;
	}
}

Matcher match(DescriptorStateRef a, DescriptorStateRef b) {
	dlg_assert(a.layout);
	dlg_assert(b.layout);

	// TODO: additional bonus matching points when the *same*
	//   ds is used? Probably bad idea.

	if(a.data == b.data) {
		// fast path: full match since same descriptorSet
		auto count = float(totalDescriptorCount(a));
		return Matcher{count, count};
	}

	// we expect them to have the same layout since they must
	// be bound for commands with the same pipeline
	dlg_assert_or(compatible(*a.layout, *b.layout), return Matcher::noMatch());

	// iterate over bindings
	Matcher m;
	for(auto bindingID = 0u; bindingID < a.layout->bindings.size(); ++bindingID) {
		// they can have different size, when variable descriptor count is used
		auto sizeA = descriptorCount(a, bindingID);
		auto sizeB = descriptorCount(b, bindingID);

		// must have the same type
		auto dsType = a.layout->bindings[bindingID].descriptorType;
		auto dsCat = vil::category(dsType);
		dlg_assert_or(a.layout->bindings[bindingID].descriptorType ==
			b.layout->bindings[bindingID].descriptorType, continue);

		if(dsType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT) {
			// This might seem like a low weight but the bytewise
			// comparison isn't the best anyways. Counting the number
			// of equal bytes or weighing this by the block size
			// would be bad.
			m.total += 1;
		} else {
			m.total += std::max(sizeA, sizeB);
		}

		// TODO: if samplers or image/buffers views are different we could
		// check them for semantic equality as well. But who would ever do something
		// as terrible as create multiple equal samplers/imageView? /s

		if(dsCat == DescriptorCategory::image) {
			auto bindingsA = images(a, bindingID);
			auto bindingsB = images(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto& bindA = bindingsA[e];
				auto& bindB = bindingsB[e];
				if(needsSampler(dsType) &&
						bindA.sampler != bindB.sampler) {
					continue;
				}

				if(needsImageView(dsType) &&
						bindA.imageView != bindB.imageView) {
					continue;
				}

				// NOTE: consider image layout? not too relevant I guess
				++m.match;
			}
		} else if(dsCat == DescriptorCategory::buffer) {
			auto bindingsA = buffers(a, bindingID);
			auto bindingsB = buffers(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto& bindA = bindingsA[e];
				auto& bindB = bindingsB[e];
				// NOTE: consider offset? not too relevant I guess
				if(bindA.buffer == bindB.buffer &&
						bindA.range == bindB.range) {
					++m.match;
				}
			}
		} else if(dsCat == DescriptorCategory::bufferView) {
			auto bindingsA = bufferViews(a, bindingID);
			auto bindingsB = bufferViews(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				if(bindingsA[e] == bindingsB[e]) {
					++m.match;
				}
			}
		} else if(dsCat == DescriptorCategory::accelStruct) {
			auto bindingsA = accelStructs(a, bindingID);
			auto bindingsB = accelStructs(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				if(bindingsA[e] == bindingsB[e]) {
					++m.match;
				}
			}
		} else if(dsCat == DescriptorCategory::inlineUniformBlock) {
			auto bytesA = inlineUniformBlock(a, bindingID);
			auto bytesB = inlineUniformBlock(b, bindingID);
			if(bytesA.size() == bytesB.size() &&
					std::memcmp(bytesA.data(), bytesB.data(), bytesA.size()) == 0) {
				++m.match;
			}
		} else {
			dlg_error("Unsupported descriptor type: {}", u32(dsType));
		}
	}

	return m;
}

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
		if(em == 0.f || em < bestMatch) {
			continue;
		}

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
// TODO: at least early-skip common begin/end. Which means this would
//   be ~O(n) for very similar batches which would be okay again.
// TODO: memory optimization: our current use of ThreadMemScope will
//   keep all memory needed during matching alive. That's bad.
//   Could recursively more local ThreadMemScopes. But then we'd have
//   to copy the selected matches on return :(
//   Maybe don't do it recursively but only once per match meta-level.
//   Copy overhead should be acceptable and it's way less of a memory issue.

// Outputs match in range [0, 1]
std::pair<span<SectionMatch>, float> match(ThreadMemScope& tms,
		const ParentCommand& rootA, const ParentCommand& rootB) {
	// match commands themselves
	const float rootMatch = eval(rootA.match(rootB));
	if(rootMatch == 0.f) {
		return {{}, 0.f};
	}

	// TODO: consider sectionStats for rootMatch.

	// setup match matrix
	struct Entry {
		float match {};
		unsigned dir {};
		float matchHere {};
		const ParentCommand* cA {};
		const ParentCommand* cB {};
		span<SectionMatch> children;
	};

	auto numSectionsA = rootA.sectionStats().numChildSections;
	auto numSectionsB = rootB.sectionStats().numChildSections;

	if(numSectionsA == 0u && numSectionsB == 0u) {
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
			auto valDiag = -1.f;
			auto [childMatches, matchVal] = match(tms, *itA, *itB);
			if(matchVal > 0) {
				valDiag = (ia == 0u || ib == 0u) ? 0.f : entry(ia - 1, ib - 1).match;
				valDiag += matchVal;
			}

			auto valUp = (ia == 0u) ? 0.f : entry(ia - 1, ib).match;
			auto valLeft = (ib == 0u) ? 0.f : entry(ia, ib - 1).match;

			auto& dst = entry(ia, ib);
			dst.match = std::max(valDiag, std::max(valUp, valLeft));
			dst.matchHere = matchVal;
			dst.children = childMatches;
			dst.cA = itA;
			dst.cB = itB;
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

			itB = itB->nextParent_;
			++ib;
		}

		itA = itA->nextParent_;
		++ia;
	}

	// backtrack
	auto maxNumMatches = std::min(numSectionsA, numSectionsB);
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

	const float resMatch = (1 + entries.back().match) / (1 + std::max(numSectionsA, numSectionsB));
	return {resMatches, rootMatch * resMatch};
}

BatchMatch match(ThreadMemScope& tms, const RecordBatch& a, const RecordBatch& b) {
	ZoneScoped;

	if(a.queue != b.queue) {
		return {0.f, &a, &b, {}};
	}

	if(a.submissions.size() == 0u && b.submissions.size() == 0u) {
		return {1.f, &a, &b, {}};
	}

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
			auto [matches, fac] = match(tms,
				*a.submissions[ia]->commands,
				*b.submissions[ib]->commands);
			auto valDiag = -1.f;
			if(fac > 0.0) {
				valDiag = (ia == 0u || ib == 0u) ? 0.f : entry(ia - 1, ib - 1).match;
				valDiag += fac;
			}

			auto valUp = (ia == 0u) ? 0.f : entry(ia - 1, ib).match;
			auto valLeft = (ib == 0u) ? 0.f : entry(ia, ib - 1).match;

			auto& dst = entry(ia, ib);
			dst.match = std::max(valDiag, std::max(valUp, valLeft));
			dst.matchHere = fac;
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

MatchResult match(ThreadMemScope& tms, span<const RecordBatch> a, span<const RecordBatch> b) {
	ZoneScoped;

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
