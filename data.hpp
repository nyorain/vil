#pragma once

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>
#include <dlg/dlg.hpp>

namespace fuen {

// data table
extern std::unordered_map<std::uint64_t, void*> dataTable;
extern std::shared_mutex dataMutex;

// We need a unique hash for type and handle id
inline std::uint64_t hash_combine(std::uint64_t a, std::uint64_t b) {
	return a ^ (b + 0x9e3779b9 + (a << 6) + (a >> 2));
}

template<typename T>
std::uint64_t handleToU64(T handle) {
	std::uint64_t id {};
	dlg_assert(sizeof(handle) <= sizeof(id));
	std::memcpy(&id, &handle, sizeof(handle));
	return id;
}

template<typename T>
std::uint64_t handleCast(T handle) {
	auto typeHash = typeid(handle).hash_code();
	return hash_combine(handleToU64(handle), typeHash);
}

template<typename T>
void* findData(T handle) {
	std::shared_lock lock(dataMutex);
	auto it = dataTable.find(handleCast(handle));
	return it == dataTable.end() ? nullptr : it->second;
}

template<typename R, typename T>
R* findData(T handle) {
	std::shared_lock lock(dataMutex);
	auto it = dataTable.find(handleCast(handle));
	return static_cast<R*>(it == dataTable.end() ? nullptr : it->second);
}

template<typename R, typename T>
R& getData(T handle) {
	std::shared_lock lock(dataMutex);
	auto it = dataTable.find(handleCast(handle));
	dlg_assert(it != dataTable.end());
	return *static_cast<R*>(it->second);
}

template<typename T>
void insertData(T handle, void* data) {
	std::lock_guard lock(dataMutex);
	auto [_, success] = dataTable.emplace(handleCast(handle), data);
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
	auto it = dataTable.find(handleCast(handle));
	if(it == dataTable.end()) {
		dlg_error("Couldn't find data for {} ({})", handleCast(handle), typeid(T).name());
		return;
	}

	dataTable.erase(it);
}

template<typename R, typename T>
std::unique_ptr<R> moveDataOpt(T handle) {
	std::lock_guard lock(dataMutex);
	auto it = dataTable.find(handleCast(handle));
	if(it == dataTable.end()) {
		return nullptr;
	}

	auto ptr = it->second;
	dataTable.erase(it);
	return std::unique_ptr<R>(static_cast<R*>(ptr));
}

template<typename R, typename T>
std::unique_ptr<R> moveData(T handle) {
	auto ret = moveDataOpt<R>(handle);
	dlg_assertm(ret, "Couldn't find data for {} ({})", handleCast(handle), typeid(T).name());
	return ret;
}

} // namespace fuen
