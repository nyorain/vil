#pragma once

#include <fwd.hpp>
#include <util/handleCast.hpp>
#include <util/dlg.hpp>
#include <vk/vulkan.h>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>

namespace vil {

// PERF(low prio): use small buffer optimization for those (especially
//   devices & instances, optimizing for the common case of just one
//   instance/device here would likely reduce the overhead quite a bit).
//   Look into using the vk_layer small-buffer hashtable implementation

// Table of all dispatchable handles (instance, device, phdev, queue, cb)
extern std::unordered_map<std::uint64_t, void*> dispatchableTable;
// Table of device loaders (the first word in any VkDevice handle, no matter
// where/how it is wrapped). This allows us in our public API implementation
// to recognize VkDevice handles directly coming from the device (we can't
// just use the dispatchableTable directly for that since it might
// be wrapped by other layers).
extern std::unordered_map<void*, Device*> devByLoaderTable;
// Synchronizes access to those global tables
extern std::shared_mutex dataMutex;

template<typename T>
void* findData(T handle) {
	std::shared_lock lock(dataMutex);
	auto it = dispatchableTable.find(handleToU64(handle));
	return it == dispatchableTable.end() ? nullptr : it->second;
}

template<typename R, typename T>
R* findData(T handle) {
	std::shared_lock lock(dataMutex);
	auto it = dispatchableTable.find(handleToU64(handle));
	return static_cast<R*>(it == dispatchableTable.end() ? nullptr : it->second);
}

template<typename R, typename T>
R& getData(T handle) {
	std::shared_lock lock(dataMutex);
	auto it = dispatchableTable.find(handleToU64(handle));
	dlg_assert(it != dispatchableTable.end());
	return *static_cast<R*>(it->second);
}

template<typename T>
void insertData(T handle, void* data) {
	std::lock_guard lock(dataMutex);
	auto [_, success] = dispatchableTable.emplace(handleToU64(handle), data);
	dlg_assert(success);
}

template<typename R, typename T, typename... Args>
R& createData(T handle, Args&&... args) {
	auto data = new R(std::forward<Args>(args)...);
	insertData(handle, data);
	return *data;
}

template<typename T>
void eraseData(T handle) {
	std::lock_guard lock(dataMutex);
	auto it = dispatchableTable.find(handleToU64(handle));
	if(it == dispatchableTable.end()) {
		dlg_error("Couldn't find data for {} ({})", handleToU64(handle), typeid(T).name());
		return;
	}

	dispatchableTable.erase(it);
}

template<typename R, typename T>
std::unique_ptr<R> moveDataOpt(T handle) {
	std::lock_guard lock(dataMutex);
	auto it = dispatchableTable.find(handleToU64(handle));
	if(it == dispatchableTable.end()) {
		return nullptr;
	}

	auto ptr = it->second;
	dispatchableTable.erase(it);
	return std::unique_ptr<R>(static_cast<R*>(ptr));
}

template<typename R, typename T>
std::unique_ptr<R> moveData(T handle) {
	auto ret = moveDataOpt<R>(handle);
	dlg_assertm(ret, "Couldn't find data for {} ({})", handleToU64(handle), typeid(T).name());
	return ret;
}

// This is only needed for the API: we get a VkDevice handle directly from
// the application.
template<typename T>
inline Device& getDeviceByLoader(T handle) {
	void* table;
	std::memcpy(&table, reinterpret_cast<void*>(handle), sizeof(table));
	std::shared_lock lock(dataMutex);
	auto it = devByLoaderTable.find(table);
	dlg_assert(it != devByLoaderTable.end());
	return *it->second;
}

inline void storeDeviceByLoader(VkDevice vkDev, Device* dev) {
	void* table;
	std::memcpy(&table, reinterpret_cast<void*>(vkDev), sizeof(table));
	std::lock_guard lock(dataMutex);
	auto [_, success] = devByLoaderTable.emplace(table, dev);
	dlg_assert(success);
}

inline void eraseDeviceFromLoaderMap(VkDevice vkDev) {
	void* table;
	std::memcpy(&table, reinterpret_cast<void*>(vkDev), sizeof(table));
	std::lock_guard lock(dataMutex);
	auto count = devByLoaderTable.erase(table);
	dlg_assert(count == 1u);
}

inline void eraseDeviceFromLoaderMap(Device& dev) {
	std::lock_guard lock(dataMutex);
	for(auto it = devByLoaderTable.begin(); it != devByLoaderTable.end(); ++it) {
		if(it->second == &dev) {
			devByLoaderTable.erase(it);
			return;
		}
	}

	dlg_error("unreachable");
}

} // namespace vil
