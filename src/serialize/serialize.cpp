#include <serialize/internal.hpp>
#include <command/record.hpp>
#include <pipe.hpp>
#include <util/dlg.hpp>

// header:
// - u32 numRecords
// - u32 numHandles
// - u32 handleTypes[numHandles]
//   (for handle type pipeline also the bind point)
// handlesBuf:
// - for (i = 0; i < numHandles; ++i): handle[i]
//   content depends on type

// We store it like this, so that when loading, we can first
// create all handles, so that we can already correctly link to them
// during loading to fill in reference fields.
// And only in a third step, we would create them on API level I guess.
// Hmpf. Complicated. But the simplest design I can come up with rn.
//
// Creating on API level is just an idea right now for serialized local
// captures, nothing that will be implemented soon.
// handles we might want to create on API level:
// - DeviceMemory
// - buffer
// - image
// - pipeline? (later on, complicated, mainly for stats e.g. vgpr pressure)

namespace vil {

// saver
StateSaverPtr createStateSaver() {
	auto ptr = StateSaverPtr{new StateSaver()};
	return ptr;
}

void destroy(StateSaver& saver) {
	delete &saver;
}

void flushPending(StateSaver& saver) {
	auto done = false;
	while(!done) {
		done = true;

		for(auto i = saver.lastWrittenHandle; i < saver.handles.size(); ++i) {
			done = false;
			writeHandle(saver, i, *saver.handles[i], saver.handleTypes[i]);
		}
		saver.lastWrittenHandle = saver.handles.size();

		for(auto i = saver.lastWrittenRecord; i < saver.records.size(); ++i) {
			done = false;
			auto& rec = const_cast<CommandRecord&>(*saver.records[i]);

			serializeMarker(saver.recordBuf, markerStartRecord + i,
				dlg::format("record {}", i));
			saveRecord(saver, saver.recordBuf, rec);
		}
		saver.lastWrittenRecord = saver.records.size();
	}
}

u64 addNoFlush(StateSaver& slz, const CommandRecord& rec) {
	auto it = std::find(slz.records.begin(), slz.records.end(), &rec);
	if(it != slz.records.end()) {
		return u64(it - slz.records.begin());
	}

	auto id = slz.records.size();
	slz.records.push_back(&rec);

	return id;
}

u64 add(StateSaver& slz, const CommandRecord& rec) {
	auto ret = addNoFlush(slz, rec);
	flushPending(slz);
	return ret;
}

const CommandRecord* getRecord(const StateSaver& saver, u64 id) {
	dlg_assert_or(id < saver.records.size(), return nullptr);
	return saver.records[id];
}

u64 getID(const StateSaver& saver, const Command& cmd) {
	auto it = saver.commandToOffset.find(&cmd);
	dlg_assert_or(it != saver.commandToOffset.end(), return u64(-1));
	return it->second;
}

u64 add(StateSaver& saver, const Handle& handle, int vkObjectType) {
	auto it = saver.handleToID.find(&handle);
	if(it != saver.handleToID.end()) {
		return it->second;
	}

	auto id = saver.handles.size();
	saver.handleToID[&handle] = id;
	saver.handles.push_back(&handle);
	saver.handleTypes.push_back(VkObjectType(vkObjectType));

	return id;
}

void write(StateSaver& saver, std::function<void(ReadBuf)> writer) {
	flushPending(saver);

	// header
	SaveBuf header;

	serializeMarker(header, markerStartData, "Start");

	write<u32>(header, saver.records.size());
	write<u32>(header, saver.handles.size());
	for(auto i = 0u; i < saver.handles.size(); ++i) {
		write(header, saver.handleTypes[i]);
		if(saver.handleTypes[i] == VK_OBJECT_TYPE_PIPELINE) {
			auto& pipe = static_cast<const Pipeline&>(*saver.handles[i]);
			write(header, pipe.type);
		}
	}

	writer(header);
	writer(saver.handleBuf);
	writer(saver.recordBuf);
}

// loader
StateLoaderPtr createStateLoader(ReadBuf rawByteBuf) {
	auto ptr = StateLoaderPtr{new StateLoader()};
	auto& loader = *ptr;
	loader.buf.buf = rawByteBuf;

	serializeMarker(loader.buf, markerStartData, "Start");

	// load header
	auto numRecords = read<u32>(loader.buf);
	auto numHandles = read<u32>(loader.buf);
	loader.handles.reserve(numHandles);
	loader.handleTypes.reserve(numHandles);
	loader.destructors.reserve(numHandles);
	for(auto i = 0u; i < numHandles; ++i) {
		addHandle(loader);
	}

	// handles
	readHandles(loader);

	// records
	loader.recordStart = loader.buf.buf.data();
	for(auto i = 0u; i < numRecords; ++i) {
		loader.records.emplace_back(new CommandRecord(manualTag, nullptr));
	}

	for(auto i = 0u; i < numRecords; ++i) {
		auto& rec = loader.records[i];
		serializeMarker(loader.buf, markerStartRecord + i,
			dlg::format("record {}", i));
		loadRecord(loader, rec, loader.buf);
	}

	dlg_assert(loader.buf.buf.empty());
	return ptr;
}

StateLoader::~StateLoader() {
	dlg_assert(destructors.size() == handles.size());
	for(auto i = 0u; i < destructors.size(); ++i) {
		dlg_assert(handles[i]);
		destructors[i](*handles[i]);
	}
}

void destroy(StateLoader& loader) {
	delete &loader;
}

IntrusivePtr<CommandRecord> getRecord(const StateLoader& loader, u64 id) {
	dlg_assertm_or(id < loader.records.size(), return nullptr, "id {}, size {}",
		id, loader.records.size());
	return loader.records[id];
}

Command* getCommand(const StateLoader& loader, u64 id) {
	auto it = loader.offsetToCommand.find(id);
	dlg_assertm_or(it != loader.offsetToCommand.end(), return nullptr,
		"id {}", id);
	return it->second;
}

Handle* getHandle(const StateLoader& loader, u64 id) {
	dlg_assert_or(id < loader.handles.size(), return nullptr);
	return loader.handles[id];
}

Handle* getHandle(const StateLoader& loader, u64 id, int vkObjectType) {
	dlg_assert_or(id < loader.handles.size(), return nullptr);
	dlg_assert_or(vkObjectType == loader.handleTypes[id], return nullptr);
	return loader.handles[id];
}

} // namespace vil
