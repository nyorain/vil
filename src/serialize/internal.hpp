#pragma once

#include <serialize/serialize.hpp>
#include <serialize/util.hpp>
#include <handle.hpp>
#include <unordered_map>
#include <vector>
#include <any>

namespace vil {

// saver
struct StateSaver {
	std::vector<const CommandRecord*> records;

	std::unordered_map<u64, const Command*> offsetToCommand;
	std::unordered_map<const Command*, u64> commandToOffset;

	std::vector<const Handle*> handles;
	std::vector<VkObjectType> handleTypes;
	std::unordered_map<const Handle*, u64> handleToID;

	u64 lastWrittenRecord {};
	u64 lastWrittenHandle {};

	DynWriteBuf recordBuf;
	DynWriteBuf handleBuf;
};

void flushPending(StateSaver& saver);

// loader
struct StateLoader {
	std::vector<IntrusivePtr<CommandRecord>> records;

	std::unordered_map<u64, Command*> offsetToCommand;
	std::unordered_map<const Command*, u64> commandToOffset;

	// NOTE: kinda ugly, need this to not leak the handles we created.
	std::vector<void(*)(Handle&)> destructors;
	std::vector<Handle*> handles;
	std::vector<VkObjectType> handleTypes;

	LoadBuf buf;
	const std::byte* recordStart {};

	u64 recordOffset() const {
		return buf.buf.data() - recordStart;
	}

	~StateLoader();
};

// like add but without flushing
u64 addNoFlush(StateSaver& slz, const CommandRecord& rec);

// commands.cpp
void loadRecord(StateLoader& loader, IntrusivePtr<CommandRecord> rec, LoadBuf& io);
void saveRecord(StateSaver& saver, SaveBuf& io, CommandRecord& rec);

// handles.cpp
void writeHandle(StateSaver& saver, u32 handleID, const Handle& handle, VkObjectType);

Handle& addHandle(StateLoader& loader);
void readHandles(StateLoader& loader);

constexpr u64 markerBase = 0xC0DEBABE00000000ull;
constexpr u64 markerStartData = markerBase + 0xABCDABCD;
constexpr u64 markerStartRecord = markerBase + 0xEC0D0000; // last bytes for record id
constexpr u64 markerStartHandle = markerBase + 0xAD1E0000; // last bytes for handle id
constexpr u64 markerStartCommand = markerBase + 0x0C0E0000; // last bytes for command type

} // namespace
