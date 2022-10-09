#pragma once

#include <fwd.hpp>
#include <nytl/span.hpp>
#include <nytl/bytes.hpp>
#include <nytl/flags.hpp>
#include <optional>
#include <string_view>

namespace vil {

// NOTE: important that destructors of Decoration and Type stay trivial as
//   we use a custom allocator that does not call them in the functions
//   below (for ease-of-use, allowing us to return just a single type allocated
//   in user-specified memory).

struct Decoration {
	enum class Bits {
		rowMajor = 1 << 0u,
		colMajor = 1 << 1u,
	};

	using Flags = nytl::Flags<Bits>;

	std::string_view name {};
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

static_assert(std::is_trivially_destructible_v<Decoration>);

// TODO: rename or move to namespace, kinda unfitting here.
// vil::bufmt::Type or something makes more sense.
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
	span<u32> array {};
	u32 width;

	Decoration deco;

	struct Member {
		std::string_view name;
		const Type* type;
		u32 offset;
	};

	span<Member> members;
};

static_assert(std::is_trivially_destructible_v<Type>);
static_assert(std::is_trivially_destructible_v<Type::Member>);

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
		LinAllocator& alloc);

struct ParsedLocation {
	unsigned line;
	unsigned col;
	std::string_view lineContent; // content of line
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

ParseTypeResult parseType(std::string_view str, LinAllocator&);

// Will simply output any errors to console.
const Type* unwrap(const ParseTypeResult& res);

} // namespace vil
