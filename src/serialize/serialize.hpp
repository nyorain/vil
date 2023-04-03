#pragma once

#include <fwd.hpp>
#include <functional>

// Small C-like interface to not clutter everything with serialization internals.

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

// = Saving =
using StateSaverPtr = std::unique_ptr<StateSaver, SerializerDeleter>;
StateSaverPtr createStateSaver();

// Adds the given CommandRecord for serialization. Returns its ids.
// Will just return the known id for a previously added record.
u64 add(StateSaver&, const CommandRecord& rec);

// Gets the record associated withthe given id.
// Returns nullptr for invalid ids.
const CommandRecord* getRecord(const StateSaver&, u64 id);

// Retrieves an ID for a given command. The CommandRecord of the given
// command must have previously been added for serialization.
u64 getID(const StateSaver&, const Command&);

// Adds the given handle for serialization, returns its id.
// Will just return the known id for a previously added handle.
u64 add(StateSaver& saver, const Handle& handle, int vkObjectType);

template<typename H>
u64 add(StateSaver& saver, const H& handle) {
	return add(saver, handle, H::objectType);
}

// Writes the serialized data out to the given function.
// The function will be called multiple times with the internal data blocks.
void write(StateSaver&, std::function<void(ReadBuf)>);


// = Loading =
using StateLoaderPtr = std::unique_ptr<StateLoader, SerializerDeleter>;

// Creating the StateLoader will consume the entire ReadBuf, loading
// all records and handles.
StateLoaderPtr createStateLoader(ReadBuf);

// Returns the record with the given id, nullptr if it does not exist.
IntrusivePtr<CommandRecord> getRecord(const StateLoader&, u64 id);

// Returns the command with the given id, nullptr if it does not exist.
// NOTE: there is currently no way to get the record associated with it.
Command* getCommand(const StateLoader&, u64 id);

// Returns the handle associated with the given id, nullptr if it does
// not exist.
Handle* getHandle(const StateLoader&, u64 id);

// Like above, but this overload additionally asserts that the handle has
// the given object type.
Handle* getHandle(const StateLoader&, u64 id, int vkObjectType);

} // namespace

