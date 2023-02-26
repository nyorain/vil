#pragma once

#include <fwd.hpp>
#include <serialize/serialize.hpp>
#include <serialize/bufs.hpp>
#include <dlg/dlg.hpp>

namespace vil {

// util
// save ref
template<typename H>
void serializeRef(StateSaver& saver, SaveBuf& buf, H& ptrRef) {
	if(!ptrRef) {
		write<u64>(buf, u64(-1));
		return;
	}
	auto id = add(saver, *ptrRef);
	write(buf, id);
}

// load ref
template<typename H>
void serializeRef(StateLoader& loader, LoadBuf& buf, H& ptrRef) {
	auto id = read<u64>(buf);
	if(id == u64(-1)) {
		ptrRef = nullptr;
		return;
	}

	using RH = std::remove_reference_t<decltype(*std::declval<H>())>;
	auto* ptr = getHandle(loader, id, unsigned(RH::objectType));
	if(!ptr) {
		// NOTE: design choice, could alternatively just assign null here
		// and maybe output a warning or something. But we want to be strict.
		throw std::runtime_error("Invalid handle ref");
	}

	ptrRef = H(static_cast<RH*>(ptr));
}

template<typename Slz, typename IO, typename C>
void serializeRefs(Slz& slz, IO& io, C& container) {
	auto serializer = [&](auto& buf, auto& ref) {
		serializeRef(slz, buf, ref);
	};
	serializeContainer(io, container, serializer);
}

template<typename T>
void assertLoadImmediate(StateLoader&, T& dst, const T& value) {
	dst = value;
}

template<typename T>
void assertLoadImmediate(StateSaver&, T& dst, const T& value) {
	dlg_assert(dst == value);
}

} // namespace
