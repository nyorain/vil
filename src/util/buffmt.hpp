#pragma once

#include <fwd.hpp>
#include <util/span.hpp>
#include <util/bytes.hpp>
#include <util/flags.hpp>
#include <spirv-cross/spirv_cross.hpp>
#include <optional>
#include <string_view>

namespace vil {

struct Decoration {
	enum class Bits {
		rowMajor = 1 << 0u,
		colMajor = 1 << 1u,
	};

	using Flags = nytl::Flags<Bits>;

	std::string name {};
	u32 offset {}; // for members; offset in struct
	// TODO: for multidim arrays, each dimension might have a non-tight
	// arrayStride in spirv i guess? In Vulkan, the stride for the
	// dimension >= 1 won't be non-tight but we might still want to
	// handle this. This currently is the array stride of the last
	// dimension (i.e. how large is one element in the array).
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
void displayTable(const char* name, const Type& type, ReadBuf data, u32 offset = 0u);

enum class BufferLayout {
	std140,
	std430,
	scalar, // TODO WIP
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
	// warnings?
};

// TODO: the returned type pointer is allocated via the ThreadMemScope. But
// the vectors inside the type tree (e.g. for array members) is not and
// will leak. Fix that (just use a span instead inside type and allocate
// that from the memscope as well?)
ParseTypeResult parseType(std::string_view str, ThreadMemScope&);

// Will simply output any errors to console.
const Type* unwrap(const ParseTypeResult& res);

} // namespace vil
