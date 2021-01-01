#pragma once

#include <fwd.hpp>
#include <commandHook.hpp>

namespace fuen {

struct IndirectCommandHook : CommandHookImpl {
	enum class Type {
		draw,
		drawCount,
		dispatch,
	};

	Type type {};
	u32 count {}; // number of commands
	std::vector<std::byte> data; // the commands

	std::unique_ptr<CommandHookRecordImpl> createRecord(CommandBuffer&, Command&) override;
	IndirectCommandHook(Type xtype) : type (xtype) {};
};

} // namespace fuen
