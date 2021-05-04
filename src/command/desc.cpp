#include <command/desc.hpp>
#include <command/commands.hpp>
#include <command/record.hpp>
#include <cb.hpp>
#include <pipe.hpp>
#include <rp.hpp>
#include <util/util.hpp>
#include <vk/enumString.hpp>
#include <tracy/Tracy.hpp>

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

bool match(const DescriptorBinding& a, const DescriptorBinding& b,
		VkDescriptorType dsType) {
	if(!a.valid || !b.valid) {
		return a.valid == b.valid;
	}

	// TODO: if samplers or image/buffers views are different we could
	// check them for semantic equality as well. But who would ever do something
	// as terrible as create multiple equal samplers/imageView? /s

	auto dsCat = category(dsType);
	if(dsCat == DescriptorCategory::image) {
		if(needsSampler(dsType) &&
				a.imageInfo.sampler != b.imageInfo.sampler) {
			return false;
		}

		if(needsImageView(dsType) &&
				a.imageInfo.imageView != b.imageInfo.imageView) {
			return false;
		}

		// NOTE: consider image layout? not too relevant I guess

		return true;
	} else if(dsCat == DescriptorCategory::bufferView) {
		return a.bufferView == b.bufferView;
	} else if(dsCat == DescriptorCategory::buffer) {
		// NOTE: consider offset? not too relevant I guess
		return a.bufferInfo.buffer == b.bufferInfo.buffer &&
			a.bufferInfo.range == b.bufferInfo.range;
	}

	dlg_error("unreachable! bogus descriptor type");
	return false;
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
		auto ba = bindings(a, bindingID);
		auto bb = bindings(b, bindingID);

		auto dsType = a.layout->bindings[bindingID].descriptorType;
		dlg_assert(a.layout->bindings[bindingID].descriptorType ==
			b.layout->bindings[bindingID].descriptorType);

		// they can have different size, when variable descriptor count is used
		count += std::max(ba.size(), bb.size());

		for(auto e = 0u; e < std::min(ba.size(), bb.size()); ++e) {
			auto& elem0 = ba[e];
			auto& elem1 = bb[e];

			if(vil::match(elem0, elem1, dsType)) {
				++match;
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
		if(m > 0.f && m > bestMatch) {
			std::vector<const Command*> restCmds;

			if(dst.size() > 1) {
				dlg_assert(it->children());
				auto newThresh = bestMatch / m;
				auto restResult = find(it->children(), dst.subspan(1), dstDsState, newThresh);
				if(restResult.hierachy.empty()) {
					continue;
				}

				restCmds = std::move(restResult.hierachy);
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

			if(m > bestMatch) {
				bestCmds.clear();
				bestCmds.push_back(it);
				bestCmds.insert(bestCmds.end(), restCmds.begin(), restCmds.end());
				bestMatch = m;
			}
		}
	}

	return {bestCmds, bestMatch};
}

} // namespace vil
