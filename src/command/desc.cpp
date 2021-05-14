#include <command/desc.hpp>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <cb.hpp>
#include <swapchain.hpp>
#include <pipe.hpp>
#include <rp.hpp>
#include <util/util.hpp>
#include <vk/enumString.hpp>
#include <util/profiling.hpp>

// TODO: the whole CommandBufferDesc code is moot, really.
// See desc2.hpp and ideas in match.md and todo.md. We might want to
// abolish the CommandGroup concepet completely, when our
// Command matching works good enough.

namespace vil {

void processType(CommandBufferDesc& desc, Command::Type type) {
	switch(type) {
		case Command::Type::draw:
			++desc.drawCommands;
			break;
		case Command::Type::dispatch:
			++desc.dispatchCommands;
			break;
		case Command::Type::sync:
			++desc.syncCommands;
			break;
		case Command::Type::transfer:
			++desc.transferCommands;
			break;
		case Command::Type::query:
			++desc.queryCommands;
			break;
		default:
			break;
	}
}

// CommandBufferDesc CommandBufferDesc::get(const Command* cmd) {
CommandBufferDesc CommandBufferDesc::getAnnotate(Command* cmd) {
	CommandBufferDesc ret;
	ret.name = "root";

	// TODO: should really use allocator
	std::unordered_map<std::string, u32> ids;

	while(cmd) {
		if(auto children = cmd->children()) {
			auto child = CommandBufferDesc::getAnnotate(children);
			child.name = cmd->nameDesc();
			ret.children.push_back(std::move(child));

			// TODO: kinda hacky, should find general mechanism
			if(auto rpc = dynamic_cast<BeginRenderPassCmd*>(cmd); rpc) {
				dlg_assert(rpc->rp);

				for(auto& attachment : rpc->rp->desc->attachments) {
					child.params.push_back(vk::name(attachment.format));
				}
			}
		}

		processType(ret, cmd->type());

		++ret.totalCommands;
		cmd->relID = ids[cmd->nameDesc()]++;
		cmd = cmd->next;
	}

	return ret;
}

float match(const CommandBufferDesc& a, const CommandBufferDesc& b) {
	// compare children
	auto bcit = b.children.begin();
	float childMatchSum = 0u;

	for(const auto& ac : a.children) {
		// NOTE: when matching children we punish different orders
		// *extremely* harshly, namely: (A, B) is considered 0% similar
		// to (B, A). Intuitively, this seems ok for command buffer
		// sections but it might be a problem in some cases; improve
		// when need arises.
		// NOTE: we only compare for exactly same sections here.
		// We could also handle the case where labels e.g. include recording-
		// specific information. Could filter out numbers or do a lexical
		// distance check or something. Revisit if need ever arises.
		for(auto it = bcit; it != b.children.end(); ++it) {
			if(it->name == ac.name && std::equal(it->params.begin(), it->params.end(),
					ac.params.begin(), ac.params.end())) {
				// NOTE: we could weigh children with more total commands
				// more, via it->totalCommands. Probably a good idea
				// NOTE: when match is low, could look forward and choose
				// child with almost full match, indicating a skip.
				childMatchSum += match(*it, ac);
				bcit = it + 1;
				break;
			}
		}
	}

	auto maxChildren = std::max(a.children.size(), b.children.size());

	// compare own commands
	float weightSum = 0.f;
	float diffSum = 0.f;

	auto addMatch = [&](u32 dst, u32 src) {
		diffSum += std::abs(float(dst) - float(src));
		weightSum += std::max(dst, src);
	};

	addMatch(a.dispatchCommands, b.dispatchCommands);
	addMatch(a.drawCommands, b.drawCommands);
	addMatch(a.transferCommands, b.transferCommands);
	addMatch(a.syncCommands, b.syncCommands);
	addMatch(a.queryCommands, b.queryCommands);

	// When there are no commands in either, we match 100%
	float ownMatch = weightSum > 0.0 ? 1.f - diffSum / weightSum : 1.f;

	// NOTE: kinda simplistic formula, can surely be improved.
	// We might want to value large setions that are similar a lot
	// more since that is a huge indicator that command buffers come from
	// the same source, even if whole sections are missing in either of them.
	// NOTE: we currently value child sections a lot since
	// we are much more interested in the *structure* of a record than
	// the actual commands (since we don't correctly match them anyways,
	// only the numbers). Should be changed when doing better per-command
	// matching
	return (ownMatch + childMatchSum) / (1 + maxChildren);
}

float match(const DescriptorSetState& a, const DescriptorSetState& b) {
	dlg_assert(a.layout);
	dlg_assert(b.layout);

	// TODO: additional bonus matching points when the *same*
	//   ds is used? Probably bad idea.

	if(&a == &b) {
		// fast path: full match since same descriptorSet
		return 1.f;
	}

	// we expect them to have the same layout since they must
	// be bound for commands with the same pipeline
	dlg_assert_or(compatible(*a.layout, *b.layout), return 0.f);

	// iterate over bindings
	unsigned count {};
	unsigned match {};
	for(auto bindingID = 0u; bindingID < a.layout->bindings.size(); ++bindingID) {
		// they can have different size, when variable descriptor count is used
		auto sizeA = descriptorCount(a, bindingID);
		auto sizeB = descriptorCount(b, bindingID);
		count += std::max(sizeA, sizeB);

		// must have the same type
		auto dsType = a.layout->bindings[bindingID].descriptorType;
		auto dsCat = vil::category(dsType);
		dlg_assert_or(a.layout->bindings[bindingID].descriptorType ==
			b.layout->bindings[bindingID].descriptorType, continue);

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
				++match;
			}
		} else if(dsCat == DescriptorCategory::buffer) {
			auto bindingsA = buffers(a, bindingID);
			auto bindingsB = buffers(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				auto& bindA = bindingsA[e];
				auto& bindB = bindingsB[e];
				// NOTE: consider offset? not too relevant I guess
				return bindA.buffer == bindB.buffer &&
					bindA.range == bindB.range;
			}
		} else if(dsCat == DescriptorCategory::bufferView) {
			auto bindingsA = bufferViews(a, bindingID);
			auto bindingsB = bufferViews(b, bindingID);
			for(auto e = 0u; e < std::min(sizeA, sizeB); ++e) {
				if(bindingsA[e] == bindingsB[e]) {
					++match;
				}
			}
		}
	}

	return float(match) / count;
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
		if(m == 0.f || m < bestMatch) {
			continue;
		}

		std::vector<const Command*> currCmds {it};

		if(dst.size() > 1) {
			dlg_assert(it->children());
			auto newThresh = bestMatch / m;
			auto restResult = find(it->children(), dst.subspan(1), dstDsState, newThresh);
			if(restResult.hierachy.empty()) {
				continue;
			}

			auto& rest = restResult.hierachy;
			currCmds.insert(currCmds.end(), rest.begin(), rest.end());
			m *= restResult.match;
		} else {
			// match descriptors, if any
			// TODO: only consider descriptors statically used by pipeline
			span<const BoundDescriptorSet> dstBound;
			span<const BoundDescriptorSet> srcBound;
			if(auto* dstCmd = dynamic_cast<const DrawCmdBase*>(dst[0])) {
				dlg_assert_or(dstCmd->state.pipe, continue);
				auto dsCount = dstCmd->state.pipe->layout->descriptors.size();
				dstBound = dstCmd->state.descriptorSets.first(dsCount);

				auto* srcCmd = dynamic_cast<const DrawCmdBase*>(it);
				dlg_assert_or(srcCmd, continue);
				dlg_assert_or(srcCmd->state.pipe == dstCmd->state.pipe, continue);
				srcBound = srcCmd->state.descriptorSets.first(dsCount);
			} else if(auto* dstCmd = dynamic_cast<const DispatchCmdBase*>(dst[0])) {
				dlg_assert_or(dstCmd->state.pipe, continue);
				auto dsCount = dstCmd->state.pipe->layout->descriptors.size();
				dstBound = dstCmd->state.descriptorSets.first(dsCount);

				auto* srcCmd = dynamic_cast<const DispatchCmdBase*>(it);
				dlg_assert_or(srcCmd, continue);
				dlg_assert_or(srcCmd->state.pipe == dstCmd->state.pipe, continue);
				srcBound = srcCmd->state.descriptorSets.first(dsCount);
			}

			if(!dstBound.empty()) {
				// TODO: consider dynamic offsets?

				unsigned match {};
				for(auto i = 0u; i < srcBound.size(); ++i) {
					if(!srcBound[i].ds || !dstBound[i].ds) {
						// TODO: not sure if this can happen. Do sets
						// that are statically not used by pipeline
						// have to be bound?
						dlg_warn("ds not bound? shouldn't happen");
						continue;
					}

					auto& src = static_cast<DescriptorSet*>(srcBound[i].ds)->state;
					auto dst = dstDsState.states.find(dstBound[i].ds);
					dlg_assert_or(dst != dstDsState.states.end(), continue);
					match += vil::match(nonNull(src), nonNull(dst->second));
				}

				m *= float(match) / srcBound.size();
			}
		}

		if(m == 0.f || m < bestMatch) {
			continue;
		} else if(m == bestMatch && !bestCmds.empty()) {
			// when the match values of two commands are equal, choose
			// simply by order in current hierachy level.
			if(std::abs(int(currCmds.front()->relID) - int(dst.front()->relID)) >=
					std::abs(int(bestCmds.front()->relID) - int(dst.front()->relID))) {
				continue;
			}
		}

		bestCmds.clear();
		bestCmds = std::move(currCmds);
		bestMatch = m;
	}

	return {bestCmds, bestMatch};
}


// WIP
// Basically just a LCS implementation, see
// https://en.wikipedia.org/wiki/Longest_common_subsequence_problem
struct MatchResult {
	std::vector<std::pair<const RecordBatch*, const RecordBatch*>> matches;
};

float match(const RecordBatch& a, const RecordBatch& b) {
	if(a.queue != b.queue) {
		return 0.f;
	}

	if(a.submissions.size() == 0u && b.submissions.size() == 0u) {
		return 1.f;
	}

	std::vector<float> entries(a.submissions.size() * b.submissions.size());
	auto entry = [&](auto ia, auto ib) -> decltype(auto) {
		return entries[ia * b.submissions.size() + ib];
	};

	for(auto ia = 0u; ia < a.submissions.size(); ++ia) {
		for(auto ib = 0u; ib < b.submissions.size(); ++ib) {
			auto fac = match(a.submissions[ia]->desc, b.submissions[ib]->desc);
			auto valDiag = (ia == 0u || ib == 0u) ? 0.f : entry(ia - 1, ib - 1) + fac;
			auto valUp = (ia == 0u) ? 0.f : entry(ia - 1, ib);
			auto valLeft = (ib == 0u) ? 0.f : entry(ia, ib - 1);

			auto& dst = entry(ia, ib);
			dst = std::max(valDiag, std::max(valUp, valLeft));
		}
	}

	return entries.back() / std::max(a.submissions.size(), b.submissions.size());
}

MatchResult match(span<const RecordBatch> a, span<const RecordBatch> b) {
	struct Entry {
		float match {};
		unsigned dir {};
	};

	std::vector<Entry> entries(a.size() * b.size());
	auto entry = [&](auto ia, auto ib) -> decltype(auto) {
		return entries[ia * b.size() + ib];
	};

	for(auto ia = 0u; ia < a.size(); ++ia) {
		for(auto ib = 0u; ib < b.size(); ++ib) {
			auto fac = match(a[ia], b[ib]);
			auto valDiag = (ia == 0u || ib == 0u) ? 0.f : entry(ia - 1, ib - 1).match + fac;
			auto valUp = (ia == 0u) ? 0.f : entry(ia - 1, ib).match;
			auto valLeft = (ib == 0u) ? 0.f : entry(ia, ib - 1).match;

			auto& dst = entry(ia, ib);
			dst.match = std::max(valDiag, std::max(valUp, valLeft));
			if(dst.match == valDiag) {
				dst.dir = 1u;
			} else if(dst.match == valUp) {
				dst.dir = 2u;
			} else if(dst.match == valLeft) {
				dst.dir = 3u;
			}
		}
	}

	// backtrack
	MatchResult res;
	auto ia = a.size();
	auto ib = b.size();
	while(ia != 0u || ib != 0u) {
		auto& src = entry(ia, ib);
		if(src.dir == 1u) {
			dlg_assert(ia > 0 && ib > 0);
			res.matches.push_back({&a[ia], &b[ib]});
			--ia;
			--ib;
		} else if(src.dir == 2u) {
			dlg_assert(ia > 0);
			--ia;
		} else if(src.dir == 3u) {
			dlg_assert(ib > 0);
			--ib;
		} else {
			dlg_fatal("unreachable");
		}
	}

	return res;
}

} // namespace vil
