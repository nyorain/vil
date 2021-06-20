#pragma once

#include <fwd.hpp>
#include <util/span.hpp>
#include <util/bytes.hpp>
#include <spirv-cross/spirv_cross.hpp>

namespace vil {

void display(const spc::Compiler& compiler, u32 typeID,
		const char* name, ReadBuf data, u32 offset = 0u);

} // namespace vil
