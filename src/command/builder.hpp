#pragma once

#include <command/record.hpp>
#include <type_traits>

namespace vil {

enum class SectionType {
	none,
	begin,
	next,
	end,
};

// Utility around building CommandRecords
struct RecordBuilder {
    struct Section {
		// command that started this section
        SectionCommand* cmd {};
		// last child command of this section that is a parent to others
        ParentCommand* lastParentChild {};
		// one level up, parent of cmd. Null only for root node
        Section* parent {};
		// might be != null even when this is the last section.
		// Re-using allocations
        Section* next {};
		// for incorrect label nesting, see docs/debug-utils-label-nesting.md
		bool pop {};
    };

	IntrusivePtr<CommandRecord> record_;
	Section* section_ {}; // the last, lowest, deepest-down section
	Command* lastCommand_ {}; // the last added command in current section (might be null)

	RecordBuilder() = default;

	// Commandbuffer construction
	RecordBuilder(CommandBuffer&);
	void reset(CommandBuffer&);

	// Commandbuffer-less construction - for loading serialized commands and testing
	RecordBuilder(Device* dev);
	void reset(Device* dev);
	void reset(IntrusivePtr<CommandRecord>);

	void appendParent(ParentCommand& cmd);
	void beginSection(SectionCommand& cmd);
	void endSection(Command* cmd);
	void append(Command& cmd);
	std::vector<const Command*> lastCommand() const;

	template<typename T, SectionType ST = SectionType::none, typename... Args>
	T& add(Args&&... args) {
		static_assert(std::is_base_of_v<Command, T>);

		// We require all commands to be trivially destructible as we
		// never destroy them. They are only meant to store data, not hold
		// ownership of anything.
		static_assert(std::is_trivially_destructible_v<T>);
		dlg_assert(record_);

		auto& cmd = record_->alloc.construct<T>(std::forward<Args>(args)...);

		if constexpr(ST == SectionType::next) {
			endSection(&cmd);
		}

		append(cmd);

		if constexpr(ST == SectionType::end) {
			endSection(&cmd);
		}

		if constexpr(ST == SectionType::begin || ST == SectionType::next) {
			static_assert(std::is_convertible_v<T*, SectionCommand*>);
			beginSection(cmd);
		}

		return cmd;
	}
};

} // namespace vil
