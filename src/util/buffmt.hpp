#pragma once

#include <fwd.hpp>
#include <util/span.hpp>
#include <util/bytes.hpp>
#include <spirv-cross/spirv_cross.hpp>

namespace vil {

void display(const spc::Compiler& compiler, u32 typeID,
		const char* name, ReadBuf data, u32 offset = 0u);

// TODO: WIP
struct Decoration {
	enum class Bits {
		rowMajor = 1 << 0u,
		colMajor = 1 << 1u,
	};

	using Flags = nytl::Flags<Bits>;

	std::string name {};
	u32 offset {}; // for members; offset in struct
	u32 arrayStride {};
	u32 matrixStride {};
	Flags flags {};
};

struct Type {
	enum BaseType {
		typeStruct,
		typeFloat,
		typeInt,
		typeUint,
		typeBool,
	};

	BaseType type;
	u32 columns {1};
	u32 vecsize {1};
	std::vector<u32> array {};
	u32 width;

	Decoration deco;

	struct Member {
		std::string name;
		const Type* type;
		u32 offset;
	};

	std::vector<Member> members;
};

void display(const char* name, const Type& type, ReadBuf data, u32 offset = 0u);

// TODO: no correct pointer/self-reference support
Type* buildType(const spc::Compiler& compiler, u32 typeID,
		ThreadMemScope& memScope);

const Type* parseType(std::string_view str, ThreadMemScope&);

} // namespace vil
