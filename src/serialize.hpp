#pragma once

#include <fwd.hpp>
#include <vector>
#include <nytl/bytes.hpp>

namespace vil {

struct StateLoader;
struct StateSaver;
void destroy(StateLoader&);
void destroy(StateSaver&);

struct SerializerDeleter {
	void operator()(StateLoader* ptr) {
		if(ptr) {
			destroy(*ptr);
		}
	}

	void operator()(StateSaver* ptr) {
		if(ptr) {
			destroy(*ptr);
		}
	}
};


// saving
using StateSaverPtr = std::unique_ptr<StateSaver, SerializerDeleter>;
StateSaverPtr createStateSaver();

u64 add(StateSaver&, const CommandRecord& rec);
const CommandRecord* getRecord(const StateSaver&, u64 id);
u64 getID(const StateSaver&, const Command&);

ReadBuf getData(StateSaver&);

// loading
using StateLoaderPtr = std::unique_ptr<StateLoader, SerializerDeleter>;
StateLoaderPtr createStateLoader(ReadBuf);

IntrusivePtr<CommandRecord> getRecord(const StateLoader&, u64 id);
Command* getCommand(const StateLoader&, u64 id);

} // namespace vil

