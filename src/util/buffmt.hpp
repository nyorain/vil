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
	// Set to 0 for runtime array.
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

enum class BufferLayout {
	std140,
	std430,
};

// TODO: returns 0u for runtime arrays atm
unsigned size(const Type& t, BufferLayout bl);
unsigned align(const Type& t, BufferLayout bl);
unsigned endAlign(const Type& t, BufferLayout bl);

// TODO: no correct pointer/self-reference support
Type* buildType(const spc::Compiler& compiler, u32 typeID,
		ThreadMemScope& memScope);

struct ParsedLocation {
	unsigned line;
	unsigned col;
	unsigned tabCount; // in addition to column
	std::string lineContent; // content of line
};

struct ParsedMessage {
	ParsedLocation loc;
	std::string message;
};

// either type or error is valid.
struct ParseTypeResult {
	const Type* type {};
	std::optional<ParsedMessage> error {};
	std::vector<ParsedMessage> warnings {};
};

const Type* parseType(std::string_view str, ThreadMemScope&);

} // namespace vil
