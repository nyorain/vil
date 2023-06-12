#pragma once

#include <fwd.hpp>
#include <nytl/bytes.hpp>
#include <nytl/tmpUtil.hpp>
#include <util/dlg.hpp>
#include <stdexcept>
#include <optional>

// NOTE: to serialize byte-buffers, prefer to use '{read,write}Container'
// or '{read,write}Bytes' explicitly, depending on what you need.
// Confusing whether raw-byte-span or container- serialization is
// used otherwise.

namespace vil {

// util
using nytl::read;
using nytl::write;

// Like ReadBuf BUT the 'read' overload below throw when out-of-range.
// We use exceptions to handle invalid/unexpected serialize inputs.
struct LoadBuf {
	nytl::ReadBuf buf;
};

using SaveBuf = DynWriteBuf;

inline void readBytes(LoadBuf& buf, WriteBuf obj) {
	if(buf.buf.size() < obj.size()) {
		throw std::out_of_range("Serialization loading issue: Out of range (read)");
	}
	nytl::read(buf.buf, obj);
}

inline void read(LoadBuf& buf, WriteBuf obj) {
	readBytes(buf, obj);
}

inline void writeBytes(SaveBuf& buf, ReadBuf obj) {
	nytl::write(buf, obj);
}

// Serialization of pointers is not allowed!
template<typename T> void read(LoadBuf& buf, T* ptr) = delete;
template<typename T> void write(SaveBuf& buf, T* ptr) = delete;

template<typename T>
void read(LoadBuf& buf, T&& dst) {
	read(buf.buf, bytes(dst));
}

template<typename T>
T read(LoadBuf& buf) {
	T obj;
	read(buf, obj);
	return obj;
}

inline void skip(LoadBuf& buf, u64 size) {
	if(buf.buf.size() < size) {
		throw std::out_of_range("Serialization loading issue: Out of range (skip)");
	}
	buf.buf = buf.buf.subspan(size);
}

// container serialization
template<typename V> using VoidEnableIfContainer = std::void_t<
		decltype(std::declval<V>().begin()),
		decltype(std::declval<V>().end()),
		decltype(std::declval<V>().size())>;
template<typename V> using VoidEnableIfReserve = std::void_t<
		decltype(std::declval<V>().reserve(0u))>;
template<typename V> using VoidEnableIfInsertContainer = std::void_t<
		decltype(std::declval<V>().begin()),
		decltype(std::declval<V>().end()),
		decltype(std::declval<V>().insert(std::declval<typename V::value_type>()))>;
template<typename V> using VoidEnableIfPushBackContainer = std::void_t<
		decltype(std::declval<V>().begin()),
		decltype(std::declval<V>().end()),
		decltype(std::declval<V>().push_back(std::declval<typename V::value_type>()))>;

template<typename T, typename Writer>
VoidEnableIfContainer<T> writeContainer(SaveBuf& buf, const T& container, Writer&& writer) {
	write<u32>(buf, container.size());
	for(auto& elem : container) {
		// const_cast for sets. Writer can expect a non-const value, but
		// must not modify it, in which case this isn't UB.
		// This is done so that writer and reader can have the same signature.
		writer(buf, const_cast<typename T::value_type&>(elem));
	}
}

template<typename T>
VoidEnableIfContainer<T> writeContainer(SaveBuf& buf, const T& container) {
	writeContainer(buf, container, [](auto& buf, auto& elem) { write(buf, elem); });
}

template<typename T, typename Reader>
VoidEnableIfContainer<T>
readContainer(LoadBuf& buf, T& container, Reader&& reader) {
	auto count = read<u32>(buf);

	dlg_assert(container.empty());
	if constexpr(validExpression<VoidEnableIfReserve>) {
		container.reserve(count);
	}

	for(auto i = 0u; i < count; ++i) {
		typename T::value_type elem;
		reader(buf, elem);

		if constexpr(validExpression<VoidEnableIfInsertContainer, T>) {
			container.insert(std::move(elem));
		} else if constexpr(validExpression<VoidEnableIfPushBackContainer, T>) {
			container.push_back(std::move(elem));
		} else {
			static_assert(templatize<T>(false));
		}
	}
}

template<typename T>
VoidEnableIfContainer<T> readContainer(LoadBuf& buf, T& container) {
	readContainer(buf, container, [](auto& buf, auto& elem) { read(buf, elem); });
}

template<typename C, typename Reader>
VoidEnableIfContainer<C> serializeContainer(LoadBuf& buf, C& container, Reader&& reader) {
	readContainer(buf, container, reader);
}

template<typename C, typename Writer>
VoidEnableIfContainer<C> serializeContainer(SaveBuf& buf, C& container, Writer&& writer) {
	writeContainer(buf, container, writer);
}

template<typename T> void read(LoadBuf& buf, std::vector<T>& ret) = delete;
template<typename T> void write(SaveBuf& buf, const std::vector<T>& ret) = delete;

// NOTE: dangerous, will only remain valid until the read buffer
// is still alive. You probably don't want to use this.
inline void read(LoadBuf& buf, std::string_view& ret) {
	auto size = read<u64>(buf);
	auto ptr = reinterpret_cast<const char*>(buf.buf.data());
	ret = std::string_view(ptr, size);
	skip(buf.buf, size);
}

inline void read(LoadBuf& buf, std::string& ret) {
	std::string_view view;
	read(buf, view);
	ret = std::string(view);
}

inline void write(SaveBuf& buf, std::string_view str) {
	write<u64>(buf, str.size());
	write(buf, span<const char>(str.data(), str.size()));
}

inline void write(SaveBuf& buf, const std::string& str) {
	write(buf, std::string_view(str));
}

template<typename T>
void serialize(SaveBuf& buf, const T& obj) {
	write(buf, obj);
}

template<typename T>
void serialize(LoadBuf& buf, T& obj) {
	read(buf, obj);
}

template<typename T>
void serialize(SaveBuf& buf, const std::optional<T>& obj) {
	write<u8>(buf, !!obj);
	if(obj) {
		write(buf, *obj);
	}
}

template<typename T>
void serialize(LoadBuf& buf, std::optional<T>& obj) {
	auto has = read<u8>(buf);
	if(has) {
		auto& dst = obj.emplace();
		read(buf, dst);
	} else {
		obj = std::nullopt;
	}
}

// Debug markers in binary serialization, could be disabled in release builds
// (but that would make serializations incompatible between debug and release).
// Costs almost nothing, so always leaving it in for now.
inline void serializeMarker(LoadBuf& buf, u64 marker, std::string_view name) {
	auto val = read<u64>(buf);
	if(val != marker) {
		dlg_error("serialize marker mismatch for {}", name);
		throw std::invalid_argument("Serialization loading error: marker mismatch");
	}
}

inline void serializeMarker(SaveBuf& buf, u64 marker, std::string_view) {
	write(buf, marker);
}

template<typename IO, typename C>
VoidEnableIfContainer<C> serializeContainer(IO& buf, C& container) {
	auto cb = [](auto& buf, auto& elem) { serialize(buf, elem); };
	serializeContainer(buf, container, cb);
}

} // namespace vil

