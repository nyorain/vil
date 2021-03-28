#include <commandDesc.hpp>
#include <commands.hpp>
#include <cb.hpp>
#include <util/util.hpp>

namespace vil {

// CommandDesc
void fillIn(CommandDesc& ret, const Command* siblings, const Command& cmd) {
	ret.command = cmd.nameDesc();
	ret.arguments = cmd.argumentsDesc();

	bool before = true;
	auto sibling = siblings;
	while(sibling) {
		if(sibling == &cmd) {
			before = false;
		}

		if(sibling->nameDesc() == ret.command) {
			++ret.count;

			if(before) {
				++ret.id;
			}
		}

		sibling = sibling->next;
	}

	dlg_assertm(!before, "Inconsistent sibling/cmd (broken command hierarchy)");
}

std::vector<CommandDesc> CommandDesc::get(const Command& root, span<const Command*> hierarchy) {
	std::vector<CommandDesc> ret;

	auto* siblingList = &root;
	for(auto* lvl : hierarchy) {
		dlg_assert(siblingList);
		fillIn(ret.emplace_back(), siblingList, *lvl);
		siblingList = lvl->children();
	}

	return ret;
}

std::vector<Command*> CommandDesc::findHierarchy(Command* root, span<const CommandDesc> desc) {
	if(desc.empty() || !root) {
		return {};
	}

	struct Candidate {
		Command* command {};
		float score {};

		bool operator<(const Candidate& other) const {
			return score < other.score;
		}
	};

	// TODO: when we can't find an exact match, we probably want to return
	// the nearest parent we could find (maybe require an even higher
	// threshold though since jumping to a false parent sucks).
	// Maybe control that behavior via an external argument
	auto findCandidates = [](Command* cmd, const CommandDesc& desc) -> std::vector<Candidate> {
		std::vector<Candidate> candidates;
		while(cmd) {
			if(cmd->nameDesc() == desc.command) {
				candidates.push_back({cmd, 0.f});
			}

			cmd = cmd->next;
		}

		for(auto c = 0u; c < candidates.size(); ++c) {
			auto& cand = candidates[c];

			auto args = cand.command->argumentsDesc();
			auto maxArgs = std::max(args.size(), desc.arguments.size());
			if(maxArgs > 0) {
				u32 numSame = 0u;
				for(auto i = 0u; i < std::min(args.size(), desc.arguments.size()); ++i) {
					if(args[i] == desc.arguments[i]) {
						++numSame;
					}
				}

				cand.score = float(numSame) / maxArgs;
			} else {
				cand.score = 1.0;
			}

			// weigh by distance
			// TODO: we can do better!
			// This is very sensitive to just erasing large chunks of data.
			// We can use desc.count to get a more precise score.
			float relDesc = float(desc.id) / desc.count;
			float relCand = float(c) / candidates.size();

			cand.score /= 1 + std::abs(5 * (relDesc - relCand));
		}

		// sort them in ascending order
		std::sort(candidates.begin(), candidates.end());

		// TODO: better filter
		auto threshold = 0.29f;
		auto cmp = [](float threshold, const auto& cand) {
			return threshold <= cand.score;
		};
		auto it = std::upper_bound(candidates.begin(), candidates.end(), threshold, cmp);
		candidates.erase(candidates.begin(), it);

		return candidates;
	};

	std::vector<Command*> ret;
	std::vector<std::vector<Candidate>> levels;
	levels.push_back(findCandidates(root, desc[0]));

	while(true) {
		if(levels.back().empty()) {
			if(ret.empty()) {
				// didn't find a matching command
				return ret;
			}

			levels.pop_back();
			ret.pop_back();
			continue;
		}

		// get the best parent candidate
		auto cand = levels.back().back();
		ret.push_back(cand.command);
		levels.back().pop_back();

		// if we are in the last level: good, just return the best
		// candidate we have found
		if(ret.size() == desc.size()) {
			return ret;
		}

		// otherwise: We must have a parent command.
		// Find all children candidates, and push them to the stack,
		// go one level deeper
		auto cands = findCandidates(cand.command->children(), desc[ret.size()]);
		levels.push_back(cands);
	}

	// dlg_error("unreachable");
}

Command* CommandDesc::find(Command* root, span<const CommandDesc> desc) {
	auto chain = findHierarchy(root, desc);
	return chain.empty() ? nullptr : chain.back();
}

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
			if(it->name == ac.name) {
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
	float childMatch = maxChildren ? childMatchSum / maxChildren : 1.f;

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
	return ownMatch * childMatch;
}

} // namespace vil
