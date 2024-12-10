#include <numeric>
#undef SPIRV_CROSS_NAMESPACE_OVERRIDE

#include "spirv_cross_parsed_ir.hpp"
#include "spirv_parser.hpp"
#include "spirv_cross.hpp"
#include <cstdio>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <algorithm>

#include <nytl/span.hpp>
#include <util/buffmt.hpp>
#include <util/linalloc.hpp>

namespace spvc {
	using namespace spirv_cross;
}

#define spc spvc

using namespace nytl;

template<typename F, bool OnSuccess = true, bool OnException = true>
class ScopeGuard {
public:
	static_assert(OnSuccess || OnException);

public:
	ScopeGuard(F&& func) :
		func_(std::forward<F>(func)),
		exceptions_(std::uncaught_exceptions()) {}

	ScopeGuard(ScopeGuard&&) = delete;
	ScopeGuard& operator =(ScopeGuard&&) = delete;

	~ScopeGuard() noexcept {
		if(exceptions_ == -1) {
			return;
		}

		try {
			auto thrown = exceptions_ < std::uncaught_exceptions();
			if((OnSuccess && !thrown) || (OnException && thrown)) {
				func_();
			}
		} catch(const std::exception& err) {
			printf("~ScopeGuard: caught exception while unwinding: %s\n", err.what());
		} catch(...) {
			printf("~ScopeGuard: caught non-exception while unwinding\n");
		}
	}

	void unset() { exceptions_ = -1; }

protected:
	F func_;
	int exceptions_;
};

// Returns ceil(num / denom), efficiently, only using integer division.
inline constexpr unsigned ceilDivide(unsigned num, unsigned denom) {
	return (num + denom - 1) / denom;
}

template<typename C>
C readFile(const char* path, bool binary) {
	assert(path);
	errno = 0;

	auto *f = std::fopen(path, binary ? "rb" : "r");
	if(!f) {
		printf("Could not open '%s' for reading: %s\n", path, std::strerror(errno));
		return {};
	}


	auto ret = std::fseek(f, 0, SEEK_END);
	if(ret != 0) {
		printf("fseek on '%s' failed: %s\n", path, std::strerror(errno));
		return {};
	}

	auto fsize = std::ftell(f);
	if(fsize < 0) {
		printf("ftell on '%s' failed: %s\n", path, std::strerror(errno));
		return {};
	}

	ret = std::fseek(f, 0, SEEK_SET);
	if(ret != 0) {
		printf("second fseek on '%s' failed: %s\n", path, std::strerror(errno));
		return {};
	}

	assert(fsize % sizeof(typename C::value_type) == 0);

	C buffer(ceilDivide(fsize, sizeof(typename C::value_type)), {});
	ret = std::fread(buffer.data(), 1, fsize, f);
	if(ret != fsize) {
		printf("fread on '%s' failed: %s\n", path, std::strerror(errno));
		return {};
	}

	return buffer;
}

using u32 = std::uint32_t;
using u16 = std::uint32_t;
template std::vector<u32> readFile<std::vector<u32>>(const char*, bool);

void writeFile(const char* path, span<const std::byte> buffer, bool binary) {
	assert(path);
	errno = 0;

	auto* f = std::fopen(path, binary ? "wb" : "w");
	if(!f) {
		// dlg_error("Could not open '{}' for writing: {}", path, std::strerror(errno));
		printf("could not open file for writing\n");
		return;
	}

	auto ret = std::fwrite(buffer.data(), 1, buffer.size(), f);
	if(ret != buffer.size()) {
		// dlg_error("fwrite on '{}' failed: {}", path, std::strerror(errno));
		printf("fwrite failed\n");
	}

	std::fclose(f);
}

struct InstrBuilder {
	spv::Op op;
	std::vector<u32> vals {0}; // first val is reserved

	void insert(std::vector<u32>& dst, u32& off) {
		assert(dst.size() >= off);
		vals[0] = u16(vals.size()) << 16 | u16(op);
		dst.insert(dst.begin() + off, vals.begin(), vals.end());
		off += vals.size();
		vals.clear();
	}

	void insert(std::vector<u32>& dst, spc::ParsedIR& ir, u32 sectionID) {
		assert(sectionID < spc::SECTION_COUNT);

		// append to end of section
		assert(sectionID != spc::SECTION_FUNCS);
		auto off = ir.section_offsets.unnamed[sectionID + 1];

		assert(dst.size() >= off);
		vals[0] = u16(vals.size()) << 16 | u16(op);
		dst.insert(dst.begin() + off, vals.begin(), vals.end());

		// update section counts
		for(auto i = sectionID + 1; i < spc::SECTION_COUNT; ++i) {
			ir.section_offsets.unnamed[i] += vals.size();
		}

		vals.clear();
	}

	template<typename T>
	std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>, InstrBuilder&>
	push(T val) {
		static_assert(sizeof(T) <= sizeof(u32));
		vals.push_back(u32(val));
		return *this;
	}

	InstrBuilder& push(std::string_view val) {
		for(auto i = 0u; i < val.size(); i += 4) {
			u32 ret = val[i];
			if(i + 1 < val.size()) ret |= val[i + 1] << 8;
			if(i + 2 < val.size()) ret |= val[i + 2] << 16;
			if(i + 3 < val.size()) ret |= val[i + 3] << 24;
			vals.push_back(ret);
		}

		return *this;
	}

	~InstrBuilder() {
		assert(vals.empty());
	}
};

using vil::Type;
using vil::Decoration;
using vil::LinAllocator;
using vil::BufferLayout;

Type* buildType(const spc::Compiler& compiler, u32 typeID,
		LinAllocator& alloc, const spc::Meta::Decoration* memberDeco) {

	auto stype = &compiler.get_type(typeID);
	if(stype->pointer) {
		dlg_assert(stype->parent_type);
		typeID = stype->parent_type;
		stype = &compiler.get_type(typeID);
	}

	auto& dst = alloc.construct<Type>();

	auto* meta = compiler.get_ir().find_meta(typeID);
	if(meta) {
		dst.deco.name = copy(alloc, meta->decoration.alias);
	}

	if(memberDeco) {
		if(memberDeco->decoration_flags.get(spv::DecorationRowMajor)) {
			dst.deco.flags |= Decoration::Bits::rowMajor;
		}
		if(memberDeco->decoration_flags.get(spv::DecorationColMajor)) {
			dst.deco.flags |= Decoration::Bits::colMajor;
		}
		if(memberDeco->decoration_flags.get(spv::DecorationMatrixStride)) {
			dst.deco.matrixStride = memberDeco->matrix_stride;
		}
		if(memberDeco->decoration_flags.get(spv::DecorationOffset)) {
			dst.deco.offset = memberDeco->offset;
		}
	}

	// handle array
	if(!stype->array.empty()) {
		dlg_assert(meta && meta->decoration.decoration_flags.get(spv::DecorationArrayStride));
		dst.deco.arrayStride = meta->decoration.array_stride;

		dlg_assert(stype->array.size() == stype->array_size_literal.size());
		dst.array = alloc.alloc<u32>(stype->array.size());

		for(auto d = 0u; d < stype->array.size(); ++d) {
			if(stype->array_size_literal[d] == true) {
				dst.array[d] = stype->array[d];
			} else {
				dst.array[d] = compiler.evaluate_constant_u32(stype->array[d]);
			}
		}

		// apparently this is needed? not entirely sure why
		dlg_assert(stype->parent_type);
		typeID = stype->parent_type;
		stype = &compiler.get_type(typeID);
		meta = compiler.get_ir().find_meta(typeID);
	}

	if(stype->basetype == spc::SPIRType::Struct) {
		// handle struct
		dst.members = alloc.alloc<Type::Member>(stype->member_types.size());
		for(auto i = 0u; i < stype->member_types.size(); ++i) {
			auto memTypeID = stype->member_types[i];

			dlg_assert(meta && meta->members.size() > i);
			auto deco = &meta->members[i];
			auto off = deco->offset;

			// TODO PERF: remove allocation via dlg format here,
			// use linearAllocator instead if needed
			auto name = dlg::format("?{}", i);
			if(!deco->alias.empty()) {
				// TODO PERF: we copy here with new, terrible
				name = deco->alias;
			}

			auto& mdst = dst.members[i];
			mdst.type = buildType(compiler, memTypeID, alloc, deco);
			mdst.name = copy(alloc, name);
			mdst.offset = off;

			if(!mdst.type) {
				return nullptr;
			}
		}

		dst.type = Type::typeStruct;
		return &dst;
	}

	// handle atom
	auto getBaseType = [](spc::SPIRType::BaseType t) -> std::optional<Type::BaseType> {
		switch(t) {
			case spc::SPIRType::Double:
			case spc::SPIRType::Float:
			case spc::SPIRType::Half:
				return Type::typeFloat;

			case spc::SPIRType::Int:
			case spc::SPIRType::Short:
			case spc::SPIRType::Int64:
			case spc::SPIRType::SByte:
				return Type::typeInt;

			case spc::SPIRType::UInt:
			case spc::SPIRType::UShort:
			case spc::SPIRType::UInt64:
			case spc::SPIRType::UByte:
				return Type::typeUint;

			case spc::SPIRType::Boolean:
				return Type::typeBool;

			default:
				return std::nullopt;
		}
	};

	auto bt = getBaseType(stype->basetype);
	if(!bt) {
		dlg_error("Unsupported shader type: {}", u32(stype->basetype));
		return nullptr;
	}

	dst.type = *bt;
	dst.width = stype->width;
	dst.vecsize = stype->vecsize;
	dst.columns = stype->columns;

	return &dst;
}

Type* buildType(const spc::Compiler& compiler, u32 typeID,
		LinAllocator& alloc) {
	return buildType(compiler, typeID, alloc, nullptr);
}

unsigned size(const Type& t, BufferLayout bl) {
	u32 arrayFac = std::accumulate(t.array.begin(), t.array.end(), 1u, std::multiplies{});
	switch(t.type) {
		case Type::typeBool:
		case Type::typeFloat:
		case Type::typeUint:
		case Type::typeInt: {
			auto vec = t.vecsize;
			if(bl == BufferLayout::std140 && vec == 3u) {
				vec = 4u;
			}
			return arrayFac * vec * t.columns * t.width / 8u;
		} case Type::typeStruct: {
			auto end = 0u;
			for(auto& member : t.members) {
				end = std::max(end, member.offset + ::size(*member.type, bl));
			}
			return arrayFac * end;
		}
	}

	dlg_error("unreachable");
	return 0u;
}

unsigned align(const Type& t, BufferLayout bl) {
	switch(t.type) {
		case Type::typeBool:
		case Type::typeFloat:
		case Type::typeUint:
		case Type::typeInt: {
			auto vec = t.vecsize;
			// For std140 *and* std430, vec3 has a 16-byte alignment
			if((bl == BufferLayout::std140 || bl == BufferLayout::std430) && vec == 3u) {
				vec = 4u;
			}
			return vec * t.width / 8u;
		} case Type::typeStruct: {
			auto ret = 0u;
			for(auto& member : t.members) {
				ret = std::max(ret, ::align(*member.type, bl));
			}
			return ret;
		}
	}

	dlg_error("unreachable");
	return 0u;
}

void outputPatched(spc::ParsedIR& ir, u32 file, u32 line) {
	auto copy = ir.spirv;
	u32 oldFuncOffset = ir.section_offsets.named.funcs;

	// set new memory addressing model
	auto& addressing = copy[ir.section_offsets.named.mem_model + 1];
	if(addressing != u32(spv::AddressingModelPhysicalStorageBuffer64)) {
		assert(addressing == u32(spv::AddressingModelLogical));
		addressing = u32(spv::AddressingModelPhysicalStorageBuffer64);
	}

	// add extension
	InstrBuilder{spv::OpExtension}
		.push("SPV_KHR_physical_storage_buffer")
		.insert(copy, ir, spc::SECTION_EXTS);

	// add capability
	InstrBuilder{spv::OpCapability}
		.push(spv::CapabilityPhysicalStorageBufferAddresses)
		.insert(copy, ir, spc::SECTION_CAPS);

	// find target position
	assert(file < ir.sources.size());
	auto& source = ir.sources[file];
	auto cmp = [](auto& a, auto& b) {
		return a.line < b;
	};
	auto lb = std::lower_bound(source.line_markers.begin(),
		source.line_markers.end(), line, cmp);
	if(lb == source.line_markers.end()) {
		printf("no matching line found\n");
		return;
	}

	if(lb->line != line) {
		printf("no exact match found: %d vs %d\n", line, lb->line);
	}

	assert(lb->function);
	const auto& name = ir.get_name(lb->function->self);
	printf("in function %s\n", name.c_str());

	vil::LinAllocator alloc;
	vil::Type baseType;
	baseType.type = vil::Type::typeStruct;

	std::vector<vil::Type::Member> members;
	std::deque<vil::Type> types;

	spc::Compiler compiler(ir);
	auto offset = 0u;

	const auto bufLayout = vil::BufferLayout::std430;
	std::vector<u32> memberTypeIDs;
	std::vector<u32> memberIDs;
	std::vector<u32> memberOffsets;

	// - build buffmt types for all the variables.
	//   OpVariable is always a pointer, basically use the pointed-to type
	//   Also, pre-filter, discard variables we cannot use
	for (auto& varID : lb->function->local_variables) {
		auto& var = ir.get<spc::SPIRVariable>(varID);

		if(var.storage != spv::StorageClassFunction || var.phi_variable) {
			continue;
		}

		auto& dstMember = members.emplace_back();
		auto* dstType = buildType(compiler, var.basetype, alloc);
		dstMember.type = dstType;
		dstMember.name = ir.get_name(varID);
		dstMember.offset = offset;

		auto dstSize = ::size(*dstType, bufLayout);

		printf(" >> %d (%d x %d): var %s: size %d\n",
			int(varID), int(dstType->type), dstType->vecsize,
			ir.get_name(varID).c_str(), dstSize);

		// NOTE: not sure if this always works. (array?)
		// Could re-declare them from scratch instead.
		// auto& srcType = ir.get<spc::SPIRType>(var.basetype);
		memberTypeIDs.push_back(var.basetype);
		memberIDs.push_back(varID);
		memberOffsets.push_back(offset);

		offset += dstSize;
	}

	// declare that struct type in spirv [patch]
	auto structID = ir.increase_bound_by(1u);
	{
		InstrBuilder builder{spv::OpTypeStruct};
		builder.push(structID);
		for(auto memberID : memberTypeIDs) {
			builder.push(memberID);
		}
		builder.insert(copy, ir, spc::SECTION_TYPES);

		// decorate member offsets
		for(auto i = 0u; i < memberOffsets.size(); ++i) {
			InstrBuilder{spv::OpMemberDecorate}
				.push(structID)
				.push(i)
				.push(spv::DecorationOffset)
				.push(memberOffsets[i])
				.insert(copy, ir, spc::SECTION_ANNOTATIONS);
		}
	}

	// declare OpTypePointer TP to that struct with PhysicalStorageBuffer [patch]
	auto pointerID = ir.increase_bound_by(1u);
	InstrBuilder{spv::OpTypePointer}
		.push(pointerID)
		.push(spv::StorageClassPhysicalStorageBuffer)
		.push(structID)
		.insert(copy, ir, spc::SECTION_TYPES);

	// - declare A, uint2 OpConstantComposite with address [patch]
	//   duplicate non-aggregate types are not allowed in spirv. Try to find
	//   the needed ones.
	u32 typeUint = u32(-1);
	u32 typeUint2 = u32(-1);

	for(auto& id : ir.ids) {
		if(id.get_type() != spc::TypeType) {
			continue;
		}

		auto& type = spc::variant_get<spc::SPIRType>(id);
		if(type.pointer || type.forward_pointer || !type.array.empty()) {
			continue;
		}

		if(type.basetype == spc::SPIRType::UInt &&
				type.columns == 1u && type.vecsize == 1u &&
				type.width == 32u) {
			assert(typeUint == u32(-1));
			typeUint = id.get_id();
		}

		if(type.basetype == spc::SPIRType::UInt &&
				type.columns == 1u && type.vecsize == 2u &&
				type.width == 32u) {
			assert(typeUint2 == u32(-1));
			typeUint2 = id.get_id();
		}
	}

	if(typeUint2 == u32(-1)) {
		if(typeUint == u32(-1)) {
			typeUint = ir.increase_bound_by(1u);
			InstrBuilder{spv::OpTypeInt}
				.push(typeUint)
				.push(32)
				.push(0)
				.insert(copy, ir, spc::SECTION_TYPES);
		}

		typeUint2 = ir.increase_bound_by(1u);
		InstrBuilder{spv::OpTypeVector}
			.push(typeUint2)
			.push(typeUint)
			.push(2)
			.insert(copy, ir, spc::SECTION_TYPES);
	}

	auto addressConstLow = ir.increase_bound_by(1u);
	auto addressConstHigh = ir.increase_bound_by(1u);
	auto addressUint2 = ir.increase_bound_by(1u);
	// auto typeOutputPtr = ir.increase_bound_by(1u);

	vil::u64 address = 0x00010002;

	InstrBuilder{spv::OpConstant}
		.push(typeUint)
		.push(addressConstLow)
		.push(u32(address))
		.insert(copy, ir, spc::SECTION_TYPES);
	InstrBuilder{spv::OpConstant}
		.push(typeUint)
		.push(addressConstHigh)
		.push(u32(address >> 32u))
		.insert(copy, ir, spc::SECTION_TYPES);
	InstrBuilder{spv::OpConstantComposite}
		.push(typeUint2)
		.push(addressUint2)
		.push(addressConstHigh)
		.push(addressConstLow)
		.insert(copy, ir, spc::SECTION_TYPES);
	// InstrBuilder{spv::OpTypePointer}
	// 	.push(typeOutputPtr)
	// 	.push(spv::StorageClassFunction)
	// 	.push(pointerID)
	// 	.insert(copy, ir, spc::SECTION_TYPES);

	//  - construct struct C via OpCompositeConstruct
	u32 instrOff = lb->offset + (ir.section_offsets.named.funcs - oldFuncOffset);
	auto srcStruct = ir.increase_bound_by(1u);
	{
		InstrBuilder builder{spv::OpCompositeConstruct};
		builder.push(structID);
		builder.push(srcStruct);
		for(auto& member : memberIDs) {
			builder.push(member);
		}
		builder.insert(copy, instrOff);
	}

	// - at dst: OpBitcast B from A to type TP
	auto dstPtr = ir.increase_bound_by(1u);
	InstrBuilder{spv::OpBitcast}
		.push(pointerID)
		.push(dstPtr)
		.push(addressUint2)
		.insert(copy, instrOff);

	//  - OpStore C to B
	InstrBuilder{spv::OpStore}
		.push(dstPtr)
		.push(srcStruct)
		.push(spv::MemoryAccessAlignedMask)
		.push(16u)
		.insert(copy, instrOff);

	// LATER:
	// - conditions: need to create new blocks, split up
	//   OpIEqual (if we want to do compare that)
	//   OpAll
	//   OpSelectionMerge
	//   OpBranchConditional
	//   OpLabel [new 1]
	//   ... (see above)
	//   OpBranch
	//   OpLabel [new 2]
	// - possibly need to declare builtins that shader did previously not need.
	//   configuring that in UI will require some thought

	// update ID bound
	copy[3] = ir.ids.size();

	writeFile("out.spv", as_bytes(span(copy)), true);
}

int main(int argc, const char** argv) {
	assert(argc > 1);
	std::vector<u32> spirv = readFile<std::vector<u32>>(argv[1], true);

	spc::Parser parser(std::move(spirv));
	parser.parse();
	auto& ir = parser.get_parsed_ir();

	outputPatched(ir, 0, 20);
}

