#include <commandDesc.hpp>
#include <commands.hpp>
#include <cb.hpp>

namespace fuen {

// CommandDesc
void fillIn(CommandDesc& ret, const CommandBuffer& cb, const Command& cmd) {
	ret.command = cmd.nameDesc();
	ret.arguments = cmd.argumentsDesc();

	auto* sibling = cb.commands;
	if(cmd.parent) {
		sibling = cmd.parent->children;
	}

	bool before = true;
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

	dlg_assert(!before);
}

std::vector<CommandDesc> CommandDesc::get(const CommandBuffer& cb, const Command& cmd) {
	dlg_assert(cb.state == CommandBuffer::State::executable);

	std::vector<CommandDesc> ret;
	fillIn(ret.emplace_back(), cb, cmd);

	// find parents
	auto* parent = cmd.parent;
	while(parent) {
		fillIn(ret.emplace_back(), cb, *parent);
		parent = parent->parent;
	}

	std::reverse(ret.begin(), ret.end());
	return ret;
}

Command* CommandDesc::find(const CommandBuffer& cb, span<const CommandDesc> desc) {
	dlg_assert(cb.state == CommandBuffer::State::executable);

	if(desc.empty()) {
		return nullptr;
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
		auto threshold = 0.5f;
		auto cmp = [](float threshold, const auto& cand) {
			return threshold <= cand.score;
		};
		auto it = std::upper_bound(candidates.begin(), candidates.end(), threshold, cmp);
		candidates.erase(candidates.begin(), it);

		return candidates;
	};

	std::vector<std::vector<Candidate>> levels;
	levels.push_back(findCandidates(cb.commands, desc[0]));

	auto i = 1u;
	while(true) {
		if(levels.back().empty()) {
			levels.pop_back();
			if(i == 1u) {
				return nullptr;
			}

			--i;
			continue;
		}

		// get the best parent candidate
		auto cand = levels.back().back();
		levels.back().pop_back();

		// if we are in the last level: nice, just return the best
		// candidate we have found
		if(i == desc.size()) {
			return cand.command;
		}

		// otherwise: we must have a parent command.
		auto sectionCmd = dynamic_cast<SectionCommand*>(cand.command);
		dlg_assert(sectionCmd);

		// Find all children candidates, and push them to the stack,
		// go one level deeper
		auto cands = findCandidates(sectionCmd->children, desc[i]);
		levels.push_back(cands);
		++i;
	}
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

CommandBufferDesc CommandBufferDesc::get(const Command* cmd) {
	CommandBufferDesc ret;
	ret.name = "root";

	while(cmd) {
		if(auto section = dynamic_cast<const SectionCommand*>(cmd); section) {
			auto child = CommandBufferDesc::get(section->children);
			child.name = section->nameDesc();
			ret.children.push_back(child);
		}

		processType(ret, cmd->type());

		++ret.totalCommands;
		cmd = cmd->next;
	}

	return ret;
}

float CommandBufferDesc::match(const Command* cmd) {
	auto cit = children.begin();

	auto foundChildren = 0u;
	auto childMatchSum = 0.f;

	CommandBufferDesc desc;

	while(cmd) {
		// try to match chilren
		if(auto section = dynamic_cast<const SectionCommand*>(cmd); section) {
			// NOTE: when matching children we punish different orders
			// *extremely* harshly, namely: (A, B) is considered 0% similar
			// to (B, A). Intuitively, this seems ok for command buffer
			// sections but it might be a problem in some cases; improve
			// when need arises.
			// NOTE: we only compare for exactly same sections here.
			// We could also handle the case where labels e.g. include recording-
			// specific information. Could filter out numbers or do a lexical
			// distance check or something. Revisit if need ever arises.
			for(auto it = cit; it != children.end(); ++it) {
				if(it->name == section->nameDesc()) {
					cit = it;
					// NOTE: we could weigh children with more total commands
					// more, via it->totalCommands. Not sure is sensible
					childMatchSum += it->match(section->children);
					++foundChildren;
					break;
				}
			}
		}

		++desc.totalCommands;
		processType(desc, cmd->type());
	}

	float childFac = float(foundChildren) / children.size();
	float avgChildMatch = foundChildren ? childMatchSum / foundChildren : 0.f;

	float weightSum = 0.f;
	float matchSum = 0.f;

	auto addMatch = [&](u32 dst, u32 src) {
		matchSum += dst - src;
		weightSum += std::max(dst, src);
	};

	addMatch(this->dispatchCommands, desc.dispatchCommands);
	addMatch(this->drawCommands, desc.drawCommands);
	addMatch(this->transferCommands, desc.transferCommands);
	addMatch(this->syncCommands, desc.syncCommands);
	addMatch(this->queryCommands, desc.queryCommands);

	// When there are no commands in either, we match 100%
	float ownMatch = weightSum > 0.0 ? matchSum / weightSum : 1.f;

	// NOTE: kinda simplistic formula, can surely be improved.
	// We might want to value large setions that are *exactly* similar a lot
	// more since that is a huge indicator that command buffers come from
	// the same source, even if whole sections are missing in either of them.
	return ownMatch * childFac * avgChildMatch;
}


} // namespace fuen
