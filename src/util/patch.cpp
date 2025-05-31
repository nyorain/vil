#include "command/alloc.hpp"
#include <util/patch.hpp>
#include <ds.hpp>
#include <rp.hpp>
#include <pipe.hpp>
#include <device.hpp>
#include <commandHook/hook.hpp>
#include <spirv_cross_parsed_ir.hpp>
#include <spirv_parser.hpp>
#include <spirv_cross.hpp>

namespace vil {

struct InstrBuilder {
	spv::Op op;
	std::vector<u32> vals {0}; // first val is reserved

	[[nodiscard]] u32 insert(std::vector<u32>& dst, u32 off) {
		assert(dst.size() >= off);
		vals[0] = u16(vals.size()) << 16 | u16(op);
		dst.insert(dst.begin() + off, vals.begin(), vals.end());
		auto ret = vals.size();
		vals.clear();
		return ret;
	}

	void insert(std::vector<u32>& dst, spc::ParsedIR::SectionOffsets& offsets, u32 sectionID) {
		assert(sectionID < spc::SECTION_COUNT);

		// append to end of section
		assert(sectionID != spc::SECTION_FUNCS);
		auto off = offsets.unnamed[sectionID + 1];

		assert(dst.size() >= off);
		vals[0] = u16(vals.size()) << 16 | u16(op);
		dst.insert(dst.begin() + off, vals.begin(), vals.end());

		// update section counts
		for(auto i = sectionID + 1; i < spc::SECTION_COUNT; ++i) {
			offsets.unnamed[i] += vals.size();
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
};

spv::ExecutionModel executionModelFromStage(VkShaderStageFlagBits stage) {
	switch(stage) {
		case VK_SHADER_STAGE_VERTEX_BIT: return spv::ExecutionModelVertex;
		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return spv::ExecutionModelTessellationControl;
		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return spv::ExecutionModelTessellationEvaluation;
		case VK_SHADER_STAGE_GEOMETRY_BIT: return spv::ExecutionModelGeometry;
		case VK_SHADER_STAGE_FRAGMENT_BIT: return spv::ExecutionModelFragment;
		case VK_SHADER_STAGE_COMPUTE_BIT: return spv::ExecutionModelGLCompute;
		case VK_SHADER_STAGE_RAYGEN_BIT_KHR: return spv::ExecutionModelRayGenerationKHR;
		case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return spv::ExecutionModelClosestHitKHR;
		case VK_SHADER_STAGE_ANY_HIT_BIT_KHR: return spv::ExecutionModelAnyHitKHR;
		case VK_SHADER_STAGE_MISS_BIT_KHR: return spv::ExecutionModelMissKHR;
		case VK_SHADER_STAGE_CALLABLE_BIT_KHR: return spv::ExecutionModelCallableKHR;
		case VK_SHADER_STAGE_INTERSECTION_BIT_KHR: return spv::ExecutionModelIntersectionKHR;
		case VK_SHADER_STAGE_MESH_BIT_EXT: return spv::ExecutionModelMeshEXT;
		case VK_SHADER_STAGE_TASK_BIT_EXT: return spv::ExecutionModelTaskEXT;
		default: return {};
	}
}

constexpr spc::Section sectionFor(spv::Op op) {
	if(op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer) {
		return spc::SECTION_TYPES;
	}

	if(op >= spv::OpConstantTrue && op <= spv::OpSpecConstantOp) {
		return spc::SECTION_TYPES;
	}

	if(op >= spv::OpDecorate && op <= spv::OpGroupMemberDecorate) {
		return spc::SECTION_ANNOTATIONS;
	}

	switch(op) {
		case spv::OpVariable: return spc::SECTION_TYPES;
		case spv::OpExtension: return spc::SECTION_EXTS;
		case spv::OpCapability: return spc::SECTION_CAPS;
		default: return spc::SECTION_COUNT;
	}
}

struct DeclInstrBuilder : InstrBuilder {
	std::vector<u32>& copy_;
	spc::ParsedIR::SectionOffsets& offsets_;
	spc::Section section_ {spc::SECTION_COUNT};

	DeclInstrBuilder(spv::Op opcode, std::vector<u32>& copy,
		spc::ParsedIR::SectionOffsets& offsets, spc::Section section) :
			InstrBuilder{opcode}, copy_(copy), offsets_(offsets), section_(section) {
	}

	~DeclInstrBuilder() {
		apply();
	}

	DeclInstrBuilder(DeclInstrBuilder&& rhs) noexcept :
			InstrBuilder(std::move(rhs)),
			copy_(rhs.copy_),
			offsets_(rhs.offsets_),
			section_(rhs.section_) {
		rhs.section_ = spc::SECTION_COUNT;
	}

	DeclInstrBuilder& operator=(DeclInstrBuilder&& rhs) = delete;

	void apply() {
		if(section_ == spc::SECTION_COUNT) {
			return;
		}

		InstrBuilder::insert(copy_, offsets_, section_);
		section_ = spc::SECTION_COUNT;
	}
};

struct FuncInstrBuilder;

struct ShaderPatch {
	constexpr static auto bufLayout = vil::BufferLayout::std430;

	const Device& dev;
	const spc::Compiler& compiler;
	std::vector<u32> copy {};
	spc::ParsedIR::SectionOffsets offsets {};
	u32 freeID {};
	u32 funcInstrOffset {};

	u32 typeBool = u32(-1);
	u32 typeFloat = u32(-1);
	u32 typeFloat4 = u32(-1);
	u32 typeUint = u32(-1);
	u32 typeUint2 = u32(-1);
	u32 typeUint3 = u32(-1);
	u32 typeBool3 = u32(-1);

	u32 const0 = u32(-1);
	u32 const1 = u32(-1);

	LinAllocator alloc {};
	std::vector<u32> addInterface {};

	template<spv::Op Op>
	DeclInstrBuilder decl() {
		constexpr auto section = sectionFor(Op);
		static_assert(section != spc::SECTION_COUNT);
		return DeclInstrBuilder(Op, copy, offsets, section);
	}

	FuncInstrBuilder instr(spv::Op op);

	template<typename... Args>
	void instr(spv::Op op, Args... args);

	template<typename... Args>
	u32 genOp(spv::Op op, u32 type, Args... args);
};

struct FuncInstrBuilder : InstrBuilder {
	ShaderPatch& patch_;
	bool written_ {};

	FuncInstrBuilder(spv::Op opcode, ShaderPatch& patch) :
		InstrBuilder{opcode}, patch_(patch) {
	}

	~FuncInstrBuilder() {
		apply();
	}

	FuncInstrBuilder(FuncInstrBuilder&& rhs) noexcept :
			InstrBuilder(std::move(rhs)),
			patch_(rhs.patch_),
			written_(rhs.written_) {
		rhs.written_ = true;
	}

	FuncInstrBuilder& operator=(FuncInstrBuilder&& rhs) = delete;

	void apply() {
		if(written_) {
			return;
		}

		u32 off = patch_.funcInstrOffset;
		u32 oldFuncOff = patch_.compiler.get_ir().section_offsets.named.funcs;
		off += (patch_.offsets.named.funcs - oldFuncOff);
		patch_.funcInstrOffset += InstrBuilder::insert(patch_.copy, off);
		written_ = true;
	}
};

FuncInstrBuilder ShaderPatch::instr(spv::Op op) {
	return {op, *this};
}

template<typename... Args>
u32 ShaderPatch::genOp(spv::Op op, u32 type, Args... args) {
	auto id = ++freeID;
	instr(op, type, id, args...);
	return id;
}

template<typename... Args>
void ShaderPatch::instr(spv::Op op, Args... args) {
	auto builder = instr(op);
	(builder.push(args), ...);
}

u32 declareBufferStruct(ShaderPatch& patch, u32 captureStruct) {
	const u32 inputStruct = ++patch.freeID;
	const u32 bufferStruct = ++patch.freeID;

	patch.decl<spv::OpTypeStruct>()
		.push(inputStruct)
		.push(patch.typeUint3)
		.push(patch.typeUint);
	patch.decl<spv::OpMemberDecorate>()
		.push(inputStruct)
		.push(0u)
		.push(spv::DecorationOffset)
		.push(0u);
	patch.decl<spv::OpMemberDecorate>()
		.push(inputStruct)
		.push(1u)
		.push(spv::DecorationOffset)
		.push(12);

	patch.decl<spv::OpTypeStruct>()
		.push(bufferStruct)
		.push(inputStruct)
		.push(captureStruct);
	patch.decl<spv::OpMemberDecorate>()
		.push(bufferStruct)
		.push(0u)
		.push(spv::DecorationOffset)
		.push(0u);
	patch.decl<spv::OpMemberDecorate>()
		.push(bufferStruct)
		.push(1u)
		.push(spv::DecorationOffset)
		.push(16u);

	return bufferStruct;
}

void findDeclareBaseTypes(ShaderPatch& patch) {
	// spir-v does not allow duplicate declarations of non-aggregate types
	patch.typeBool = u32(-1);
	patch.typeFloat = u32(-1);
	patch.typeUint = u32(-1);
	patch.typeUint2 = u32(-1);
	patch.typeUint3 = u32(-1);
	patch.typeFloat4 = u32(-1);

	for(auto& id : patch.compiler.get_ir().ids) {
		if(id.get_type() != spc::TypeType) {
			continue;
		}

		auto& type = spc::variant_get<spc::SPIRType>(id);
		if(type.pointer || type.forward_pointer || !type.array.empty()) {
			continue;
		}

		// scalar
		if(type.basetype == spc::SPIRType::UInt &&
				type.columns == 1u && type.vecsize == 1u &&
				type.width == 32u) {
			dlg_assert(patch.typeUint == u32(-1));
			patch.typeUint = id.get_id();
		}

		if(type.basetype == spc::SPIRType::Boolean &&
				type.columns == 1u && type.vecsize == 1u) {
			dlg_assert(patch.typeBool == u32(-1));
			patch.typeBool = id.get_id();
		}

		if(type.basetype == spc::SPIRType::Float &&
				type.columns == 1u && type.vecsize == 1u &&
				type.width == 32u) {
			dlg_assert(patch.typeFloat == u32(-1));
			patch.typeFloat = id.get_id();
		}

		// vector
		if(type.basetype == spc::SPIRType::Float &&
				type.columns == 1u && type.vecsize == 4u &&
				type.width == 32u) {
			dlg_assert(patch.typeFloat4 == u32(-1));
			patch.typeFloat4 = id.get_id();
		}
		if(type.basetype == spc::SPIRType::UInt &&
				type.columns == 1u && type.vecsize == 2u &&
				type.width == 32u) {
			dlg_assert(patch.typeUint2 == u32(-1));
			patch.typeUint2 = id.get_id();
		}
		if(type.basetype == spc::SPIRType::UInt &&
				type.columns == 1u && type.vecsize == 3u &&
				type.width == 32u) {
			dlg_assert(patch.typeUint3 == u32(-1));
			patch.typeUint3 = id.get_id();
		}
		if(type.basetype == spc::SPIRType::Boolean &&
				type.columns == 1u && type.vecsize == 3u &&
				type.width == 32u) {
			dlg_assert(patch.typeBool3 == u32(-1));
			patch.typeBool3 = id.get_id();
		}
	}

	// scalars
	if(patch.typeBool == u32(-1)) {
		patch.typeBool = ++patch.freeID;
		patch.decl<spv::OpTypeBool>().push(patch.typeBool);
	}
	if(patch.typeUint == u32(-1)) {
		patch.typeUint = ++patch.freeID;
		patch.decl<spv::OpTypeInt>()
			.push(patch.typeUint)
			.push(32)
			.push(0);
	}
	if(patch.typeFloat == u32(-1)) {
		patch.typeFloat = ++patch.freeID;
		patch.decl<spv::OpTypeFloat>()
			.push(patch.typeFloat)
			.push(32);
	}
	// vectors
	if(patch.typeBool3 == u32(-1)) {
		patch.typeBool3 = ++patch.freeID;
		patch.decl<spv::OpTypeVector>()
			.push(patch.typeBool3)
			.push(patch.typeBool)
			.push(3);
	}

	if(patch.typeUint2 == u32(-1)) {
		patch.typeUint2 = ++patch.freeID;
		patch.decl<spv::OpTypeVector>()
			.push(patch.typeUint2)
			.push(patch.typeUint)
			.push(2);
	}
	if(patch.typeUint3 == u32(-1)) {
		patch.typeUint3 = ++patch.freeID;
		patch.decl<spv::OpTypeVector>()
			.push(patch.typeUint3)
			.push(patch.typeUint)
			.push(3);
	}
	if(patch.typeFloat4 == u32(-1)) {
		patch.typeFloat4 = ++patch.freeID;
		patch.decl<spv::OpTypeVector>()
			.push(patch.typeFloat4)
			.push(patch.typeFloat)
			.push(4);
	}
}

void declareConstants(ShaderPatch& patch) {
	patch.const0 = ++patch.freeID;
	patch.const1 = ++patch.freeID;
	patch.decl<spv::OpConstant>().push(patch.typeUint).push(patch.const0).push(0);
	patch.decl<spv::OpConstant>().push(patch.typeUint).push(patch.const1).push(1);
}

struct VariableCapture {
	u32 varID;
	u32 typeID;
	Type* parsed;

	bool isPointer {};
	u32 offset {};
};

bool supportedForCapture(ShaderPatch& patch, const spc::SPIRType& type) {
	(void) patch;
	return type.basetype >= spc::SPIRType::Boolean &&
		type.basetype != spc::SPIRType::AtomicCounter &&
		type.basetype <= spc::SPIRType::Struct;
}

void fixDecorateCaptureType(ShaderPatch& patch, Type& type) {
	const auto& ir = patch.compiler.get_ir();
	if(!type.members.empty()) {
		dlg_assert(type.type == Type::typeStruct);

		auto* meta = ir.find_meta(type.deco.typeID);
		dlg_assert(meta && meta->members.size() == type.members.size());
		auto needsOffsetDeco = !meta->members[0].decoration_flags.get(spv::DecorationOffset);
		auto offset = 0u;

		for(auto [i, member] : enumerate(type.members)) {
			fixDecorateCaptureType(patch, *const_cast<Type*>(member.type));

			if(needsOffsetDeco) {
				dlg_assert(!meta->members[0].decoration_flags.get(spv::DecorationOffset));
				offset = vil::alignPOT(offset, align(type, patch.bufLayout));
				member.offset = offset;

				patch.decl<spv::OpMemberDecorate>()
					.push(type.deco.typeID)
					.push(u32(i))
					.push(spv::DecorationOffset)
					.push(offset);

				auto dstSize = size(*member.type, patch.bufLayout);
				offset += dstSize;
			}
		}
	}

	if(!type.array.empty()) {
		dlg_assert(type.deco.arrayTypeID != 0u);
		auto* meta = ir.find_meta(type.deco.arrayTypeID);
		if(!meta || !meta->decoration.decoration_flags.get(spv::DecorationArrayStride)) {
			dlg_assert(type.deco.arrayStride == 0u);

			auto tarray = type.array;
			type.array = {};
			type.deco.arrayStride = align(
				size(type, patch.bufLayout),
				align(type, patch.bufLayout));
			type.array = tarray;

			patch.decl<spv::OpDecorate>()
				.push(type.deco.arrayTypeID)
				.push(spv::DecorationArrayStride)
				.push(type.deco.arrayStride);
		} else {
			dlg_assert(type.deco.arrayStride);
		}
	}

	// TODO: matrixStride
	if(type.columns > 1u) {
		dlg_error("TODO: add matrixstride deco");
	}
}

u32 findBuiltin(ShaderPatch& patch,
		const spc::SPIREntryPoint& entryPoint, spv::BuiltIn builtinType) {
	auto& compiler = patch.compiler;

	u32 ret = u32(-1);
	for(auto& builtin : compiler.get_shader_resources().builtin_inputs) {
		if(builtin.builtin == builtinType) {
			auto varID = builtin.resource.id;
			if(contains(entryPoint.interface_variables, varID)) {
				ret = varID;
				break;
			}
		}
	}

	return ret;
}

std::pair<u32, u32> findOrDeclareBuiltinInput(ShaderPatch& patch,
		const spc::SPIREntryPoint& entryPoint, spv::BuiltIn builtinType, u32 type) {
	u32 id = findBuiltin(patch, entryPoint, builtinType);
	if(id != u32(-1)) {
		auto ptype = patch.compiler.get_ir().get<spc::SPIRVariable>(id).basetype;
		auto& typeInfo = patch.compiler.get_ir().get<spc::SPIRType>(ptype);
		dlg_assert(typeInfo.pointer);
		return {id, u32(typeInfo.parent_type)};
	}

	auto pointerType = ++patch.freeID;

	patch.decl<spv::OpTypePointer>()
		.push(pointerType)
		.push(spv::StorageClassInput)
		.push(type);

	auto varID = ++patch.freeID;
	patch.decl<spv::OpVariable>()
		.push(pointerType)
		.push(varID)
		.push(spv::StorageClassInput);

	patch.decl<spv::OpDecorate>()
		.push(varID)
		.push(spv::DecorationBuiltIn)
		.push(builtinType);

	patch.addInterface.push_back(varID);

	return {varID, type};
}

u32 loadBuiltinInput(ShaderPatch& patch,
		const spc::SPIREntryPoint& entryPoint, spv::BuiltIn builtinType, u32 dstType) {
	auto [varID, varType] = findOrDeclareBuiltinInput(patch, entryPoint, builtinType, dstType);
	auto loaded = patch.genOp(spv::OpLoad, varType, varID);

	if(dstType == varType) {
		return loaded;
	}

	// NOTE: only valid for signed conversation when value is unsigned anyways
	return patch.genOp(spv::OpBitcast, dstType, loaded);
}

u32 generateInvocationCompute(ShaderPatch& patch,
		const spc::SPIREntryPoint& entryPoint) {
	return loadBuiltinInput(patch, entryPoint,
		spv::BuiltInGlobalInvocationId, patch.typeUint3);
}

u32 generateInvocationVertex(ShaderPatch& patch,
		const spc::SPIREntryPoint& entryPoint) {
	auto drawIndex = patch.const0;
	if(patch.dev.shaderDrawParameters) {
		drawIndex = loadBuiltinInput(patch, entryPoint,
			spv::BuiltInDrawIndex, patch.typeUint);

		auto& ir = patch.compiler.get_ir();

		auto extName = "SPV_KHR_shader_draw_parameters";
		if(!contains(ir.declared_extensions, extName)) {
			patch.decl<spv::OpExtension>().push(extName);
		}

		auto capName = spv::CapabilityDrawParameters;
		if(!contains(ir.declared_capabilities, capName)) {
			patch.decl<spv::OpCapability>() .push(capName);
		}
	}
	auto instanceIndex = loadBuiltinInput(patch, entryPoint,
		spv::BuiltInInstanceIndex, patch.typeUint);
	auto vertexIndex = loadBuiltinInput(patch, entryPoint,
		spv::BuiltInVertexIndex, patch.typeUint);

	auto composited = patch.genOp(spv::OpCompositeConstruct, patch.typeUint3,
		drawIndex, instanceIndex, vertexIndex);

	return composited;
}

u32 generateInvocationFragment(ShaderPatch& patch,
		const spc::SPIREntryPoint& entryPoint) {
	auto [posVar, posType] = findOrDeclareBuiltinInput(patch, entryPoint,
		spv::BuiltInFragCoord, patch.typeFloat4);
	dlg_assert(posType == patch.typeFloat4);
	(void) posType;

	auto pos = patch.genOp(spv::OpLoad, patch.typeFloat4, posVar);

	u32 fx = patch.genOp(spv::OpCompositeExtract, patch.typeFloat, pos, 0);
	u32 fy = patch.genOp(spv::OpCompositeExtract, patch.typeFloat, pos, 1);
	u32 ux = patch.genOp(spv::OpConvertFToU, patch.typeUint, fx);
	u32 uy = patch.genOp(spv::OpConvertFToU, patch.typeUint, fy);
	u32 composite = patch.genOp(spv::OpCompositeConstruct, patch.typeUint3,
		ux, uy, patch.const0);

	return composite;
}

u32 generateInvocationRaytrace(ShaderPatch& patch,
		const spc::SPIREntryPoint& entryPoint) {
	return loadBuiltinInput(patch, entryPoint,
		spv::BuiltInLaunchIdKHR, patch.typeUint3);
}

u32 generateCurrentInvocation(ShaderPatch& patch,
		const spc::SPIREntryPoint& entryPoint, VkShaderStageFlagBits stage) {
	switch(stage) {
		case VK_SHADER_STAGE_VERTEX_BIT:
			return generateInvocationVertex(patch, entryPoint);
		case VK_SHADER_STAGE_FRAGMENT_BIT:
			return generateInvocationFragment(patch, entryPoint);
		case VK_SHADER_STAGE_COMPUTE_BIT:
			return generateInvocationCompute(patch, entryPoint);
		case VK_SHADER_STAGE_RAYGEN_BIT_KHR:
		case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
		case VK_SHADER_STAGE_ANY_HIT_BIT_KHR:
		case VK_SHADER_STAGE_MISS_BIT_KHR:
		case VK_SHADER_STAGE_CALLABLE_BIT_KHR:
		case VK_SHADER_STAGE_INTERSECTION_BIT_KHR:
			return generateInvocationCompute(patch, entryPoint);
		default:
			dlg_error("unsupported shader stage");
			return {};
	}
}

PatchResult patchShaderCapture(const Device& dev, const spc::Compiler& compiler,
		u32 file, u32 line,
		u64 captureAddress, const std::string& entryPointName,
		VkShaderStageFlagBits stage) {
	auto& ir = compiler.get_ir();
	ShaderPatch patch{dev, compiler};

	// get entry point
	auto executionModel = executionModelFromStage(stage);
	auto& entryPoint = compiler.get_entry_point(entryPointName, executionModel);

	// find target position
	assert(file < ir.sources.size());
	auto& source = ir.sources[file];
	auto cmp = [](auto& a, auto& b) {
		return a.line < b;
	};
	auto lb = std::lower_bound(source.line_markers.begin(),
		source.line_markers.end(), line, cmp);
	if(lb == source.line_markers.end()) {
		dlg_debug("no matching line found");
		return {};
	}

	if(lb != source.line_markers.begin()) {
		auto before = lb;
		before--;
		if(lb->line - line > line - before->line) {
			lb = before;
		}
	}

	if(lb->line != line) {
		dlg_debug("no exact match found: {} vs {}", line, lb->line);
	}

	assert(lb->function);

	patch.copy = ir.spirv;
	patch.offsets = ir.section_offsets;
	patch.freeID = u32(ir.ids.size());

	auto& copy = patch.copy;
	auto& freeID = patch.freeID;

	// set new memory addressing model
	auto& addressing = copy[ir.section_offsets.named.mem_model + 1];
	if(addressing != u32(spv::AddressingModelPhysicalStorageBuffer64)) {
		dlg_assertm(addressing == u32(spv::AddressingModelLogical),
			"Unexpected addressing mode {}", u32(addressing));
		addressing = u32(spv::AddressingModelPhysicalStorageBuffer64);
	}

	findDeclareBaseTypes(patch);
	declareConstants(patch);

	// allow us to use physical storage buffer pointers
	auto extName = "SPV_KHR_physical_storage_buffer";
	if(!contains(ir.declared_extensions, extName)) {
		patch.decl<spv::OpExtension>().push(extName);
	}

	auto capName = spv::CapabilityPhysicalStorageBufferAddresses;
	if(!contains(ir.declared_capabilities, capName)) {
		patch.decl<spv::OpCapability>() .push(capName);
	}

	// parse local variables
	vil::Type baseType;
	baseType.type = vil::Type::typeStruct;

	LinAllocVector<vil::Type::Member> members(patch.alloc);
	members.reserve(lb->function->local_variables.size());

	auto offset = 0u;
	std::vector<VariableCapture> captures;

	auto addCapture = [&](u32 varID, u32 typeID, bool isPointer, const std::string& name) {
		auto* parsedType = buildType(patch.compiler, typeID, patch.alloc);
		fixDecorateCaptureType(patch, *parsedType);

		offset = alignPOT(offset, align(*parsedType, patch.bufLayout));

		auto& capture = captures.emplace_back();
		capture.parsed = parsedType;
		capture.typeID = typeID;
		capture.varID = varID;
		capture.offset = offset;
		capture.isPointer = isPointer;

		auto& member = members.emplace_back();
		member.type = capture.parsed;
		member.offset = offset;
		member.name = copyString(patch.alloc, name);

		auto dstSize = size(*capture.parsed, patch.bufLayout);
		offset += dstSize;
	};

	// capture local variables
	for(auto& varID : lb->function->local_variables) {
		auto& var = ir.get<spc::SPIRVariable>(varID);
		if(var.storage != spv::StorageClassFunction || var.phi_variable) {
			continue;
		}

		auto name = ir.get_name(varID);
		if(!var.debugLocalVariables.empty()) {
			auto& lvar = ir.get<spc::SPIRDebugLocalVariable>(var.debugLocalVariables.front());
			name = ir.get<spc::SPIRString>(lvar.nameID).str;
		}

		if(name.empty()) {
			continue;
		}

		auto& srcType = ir.get<spc::SPIRType>(var.basetype);
		if(!supportedForCapture(patch, srcType)) {
			continue;
		}

		dlg_assert(srcType.pointer);
		dlg_assert(srcType.pointer_depth == 1u);

		addCapture(varID, srcType.parent_type, true, name);
	}

	// capture function arguments
	for(auto& param : lb->function->arguments) {
		auto name = ir.get_name(param.id);
		if(name.empty()) {
			continue;
		}

		auto& srcType = ir.get<spc::SPIRType>(param.type);
		if(!supportedForCapture(patch, srcType)) {
			continue;
		}

		auto typeID = param.type;
		bool isPointer = false;
		if(srcType.pointer) {
			dlg_assert(srcType.pointer_depth == 1u);
			typeID = srcType.parent_type;
			isPointer = true;
		}

		addCapture(param.id, typeID, isPointer, name);
	}

	// declare that struct type in spirv [patch]
	auto captureStruct = ++freeID;

	{
		auto builder = patch.decl<spv::OpTypeStruct>();
		builder.push(captureStruct);
		for(auto& capture : captures) {
			builder.push(capture.typeID);
		}
	}

	// decorate member offsets
	for(auto [i, capture] : enumerate(captures)) {
		patch.decl<spv::OpMemberDecorate>()
			.push(captureStruct)
			.push(u32(i))
			.push(spv::DecorationOffset)
			.push(capture.offset);
	}

	u32 bufferStruct = declareBufferStruct(patch, captureStruct);

	// declare OpTypePointer TP to that struct with PhysicalStorageBuffer [patch]
	auto bufferPointerType = ++freeID;
	patch.decl<spv::OpTypePointer>()
		.push(bufferPointerType)
		.push(spv::StorageClassPhysicalStorageBuffer)
		.push(bufferStruct);

	// - declare A, uint2 OpConstantComposite with address [patch]
	//   duplicate non-aggregate types are not allowed in spirv. Try to find
	//   the needed ones.
	auto addressConstLow = ++freeID;
	auto addressConstHigh = ++freeID;
	auto addressUint2 = ++freeID;
	// auto typeOutputPtr = ir.increase_bound_by(1u);

	patch.decl<spv::OpConstant>()
		.push(patch.typeUint)
		.push(addressConstLow)
		.push(u32(captureAddress));
	patch.decl<spv::OpConstant>()
		.push(patch.typeUint)
		.push(addressConstHigh)
		.push(u32(captureAddress >> 32u));
	patch.decl<spv::OpConstantComposite>()
		.push(patch.typeUint2)
		.push(addressUint2)
		.push(addressConstLow)
		.push(addressConstHigh);

	// find builtin GlobalInvocationID
	//  - construct struct C via OpCompositeConstruct
	patch.funcInstrOffset = lb->offset;
	auto srcStruct = ++freeID;
	{
		auto builder = patch.instr(spv::OpCompositeConstruct);
		builder.push(captureStruct);
		builder.push(srcStruct);
		for(auto [i, capture] : enumerate(captures)) {
			u32 memID {};

			if(capture.isPointer) {
				memID = ++freeID;
				patch.instr(spv::OpLoad)
					.push(capture.typeID)
					.push(memID)
					.push(capture.varID);
			} else {
				memID = capture.varID;
			}

			builder.push(memID);
		}
	}

	auto constScopeDevice = ++freeID;
	auto constMemorySemanticsBuf = ++freeID;
	patch.decl<spv::OpConstant>().
		push(patch.typeUint).
		push(constScopeDevice).
		push(u32(spv::ScopeDevice));
	patch.decl<spv::OpConstant>().
		push(patch.typeUint).
		push(constMemorySemanticsBuf).
		push(u32(spv::MemorySemanticsMaskNone | spv::MemorySemanticsUniformMemoryMask));

	auto bufferPointer = patch.genOp(spv::OpBitcast, bufferPointerType, addressUint2);

	// access chains
	auto inputThreadAccessType = ++freeID;
	patch.decl<spv::OpTypePointer>()
		.push(inputThreadAccessType)
		.push(spv::StorageClassPhysicalStorageBuffer)
		.push(patch.typeUint3);

	auto inputThreadAccess = ++freeID;
	patch.instr(spv::OpInBoundsAccessChain)
		.push(inputThreadAccessType)
		.push(inputThreadAccess)
		.push(bufferPointer)
		.push(patch.const0)
		.push(patch.const0);

	auto ioCounterAccessType = ++freeID;
	patch.decl<spv::OpTypePointer>()
		.push(ioCounterAccessType)
		.push(spv::StorageClassPhysicalStorageBuffer)
		.push(patch.typeUint);

	auto ioCounterAccess = ++freeID;
	patch.instr(spv::OpInBoundsAccessChain)
		.push(ioCounterAccessType)
		.push(ioCounterAccess)
		.push(bufferPointer)
		.push(patch.const0)
		.push(patch.const1);

	// ==========
	auto blockWrite = ++freeID;
	auto blockRest = ++freeID;

	auto writingInvocation = patch.genOp(spv::OpLoad, patch.typeUint3,
		inputThreadAccess, spv::MemoryAccessAlignedMask, 16u);
	auto currentInvocation = generateCurrentInvocation(patch,
		entryPoint, stage);
	auto vecEqual = patch.genOp(spv::OpIEqual, patch.typeBool3,
		writingInvocation, currentInvocation);
	auto allEqual = patch.genOp(spv::OpAll, patch.typeBool, vecEqual);

	patch.instr(spv::OpSelectionMerge, blockRest, spv::SelectionControlMaskNone);
	patch.instr(spv::OpBranchConditional, allEqual, blockWrite, blockRest);

	// block for writing output
	{
		patch.instr(spv::OpLabel, blockWrite);

		auto captureAccessType = ++freeID;
		patch.decl<spv::OpTypePointer>()
			.push(captureAccessType)
			.push(spv::StorageClassPhysicalStorageBuffer)
			.push(captureStruct);

		auto captureAccess = ++freeID;
		patch.instr(spv::OpInBoundsAccessChain)
			.push(captureAccessType)
			.push(captureAccess)
			.push(bufferPointer)
			.push(patch.const1);

		// atomic counter
		auto oldCounter = ++freeID;
		patch.instr(spv::OpAtomicIIncrement)
			.push(patch.typeUint)
			.push(oldCounter)
			.push(ioCounterAccess)
			.push(constScopeDevice)
			.push(constMemorySemanticsBuf);

		// TODO: allow values other than 0. Configure via UI and
		// load from capture buffer as input here.
		auto counterEqual = ++freeID;
		patch.instr(spv::OpIEqual)
			.push(patch.typeBool)
			.push(counterEqual)
			.push(oldCounter)
			.push(patch.const0);

		auto blockNoWrite = ++freeID; // spirv does not allow re-using merge block
		auto blockOutput = ++freeID;
		patch.instr(spv::OpSelectionMerge)
			.push(blockNoWrite)
			.push(spv::SelectionControlMaskNone);
		patch.instr(spv::OpBranchConditional)
			.push(counterEqual)
			.push(blockOutput)
			.push(blockNoWrite);

		{
			patch.instr(spv::OpLabel).push(blockOutput);
			patch.instr(spv::OpStore)
				.push(captureAccess)
				.push(srcStruct)
				.push(spv::MemoryAccessAlignedMask)
				.push(16u);
			patch.instr(spv::OpBranch, blockNoWrite);
		}

		patch.instr(spv::OpLabel, blockNoWrite);
		patch.instr(spv::OpBranch, blockRest);
	}

	// rest of the current block
	patch.instr(spv::OpLabel, blockRest);

	// update interface of entry point
	auto eOffset = patch.offsets.named.entry_points - ir.section_offsets.named.entry_points;
	auto& instrHead = patch.copy[entryPoint.offset + eOffset];
	auto numWords = (instrHead >> 16u) & 0xFFFFu;
	numWords += u32(patch.addInterface.size());
	dlg_assert(numWords < (1u << 16u));
	instrHead = (numWords << 16u) | (instrHead & 0xFFFFu);

	auto ioffset = entryPoint.interface_offset + eOffset;
	patch.copy.insert(patch.copy.begin() + ioffset,
		patch.addInterface.begin(), patch.addInterface.end());

	// update ID bound
	copy[3] = freeID + 1;

	PatchResult res;
	res.alloc = std::move(patch.alloc);
	res.copy = std::move(copy);
	res.captures = members;

	return res;
}

vku::Pipeline createPatchCopy(const ComputePipeline& src, VkShaderStageFlagBits stage,
		span<const u32> patchedSpv) {
	dlg_assert(stage == VK_SHADER_STAGE_COMPUTE_BIT);

	dlg_assert(src.stage.spirv);
	auto& dev = *src.dev;
	auto specInfo = src.stage.specialization.vkInfo();

	vku::ShaderModule mod(dev, patchedSpv);

	VkComputePipelineCreateInfo cpi {};
	cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpi.layout = src.layout->handle;
	cpi.stage.pSpecializationInfo = &specInfo;
	cpi.stage.stage = stage;
	cpi.stage.pName = src.stage.entryPoint.c_str();
	cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	cpi.stage.module = mod.vkHandle();

	return vku::Pipeline(dev, cpi);
}

vku::Pipeline createPatchCopy(const GraphicsPipeline& src, VkShaderStageFlagBits stage,
		span<const u32> patchedSpv) {
	auto& dev = *src.dev;

	VkGraphicsPipelineCreateInfo gpi {};
	gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpi.layout = src.layout->handle;
	gpi.pRasterizationState = &src.rasterizationState;

	if(!src.hasMeshShader) {
		if(!src.dynamicState.count(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) {
			gpi.pVertexInputState = &src.vertexInputState;
		}
		gpi.pInputAssemblyState = &src.inputAssemblyState;
	}

	if(!src.rasterizationState.rasterizerDiscardEnable) {
		gpi.pMultisampleState = &src.multisampleState;
		gpi.pViewportState = &src.viewportState;

		if(src.hasDepthStencil) {
			gpi.pDepthStencilState = &src.depthStencilState;
		}
	}

	if(src.hasTessellation) {
		gpi.pTessellationState = &src.tessellationState;
	}

	if(src.needsColorBlend) {
		gpi.pColorBlendState = &src.colorBlendState;
	}

	std::vector<VkDynamicState> dynStates {
		src.dynamicState.begin(), src.dynamicState.end()};
	VkPipelineDynamicStateCreateInfo dynState {};
	dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynState.dynamicStateCount = dynStates.size();
	dynState.pDynamicStates = dynStates.data();
	gpi.pDynamicState = &dynState;

	std::vector<VkPipelineShaderStageCreateInfo> stages;
	std::vector<VkSpecializationInfo> specInfos;
	std::vector<vku::ShaderModule> mods;

	specInfos.reserve(src.stages.size());

	for(auto& srcStage : src.stages) {
		// Create shader module
		// We even do it for non-patched shaders, as the original modules
		// might have been destroyed.
		span<const u32> spirv;

		if(srcStage.stage == stage) {
			spirv = patchedSpv;
		} else {
			// normally, we use a mutex to access 'compiled'. But here
			// we only access the immutable spirv so it is not needed.
			spirv = srcStage.spirv->compiled->get_ir().spirv;
		}

		auto& mod = mods.emplace_back(dev, spirv);

		// add stage

		auto& dstStage = stages.emplace_back();
		dstStage = {};
		dstStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		dstStage.stage = srcStage.stage;
		dstStage.pName = srcStage.entryPoint.c_str();
		dstStage.module = mod.vkHandle();
		dstStage.pSpecializationInfo =
			&specInfos.emplace_back(srcStage.specialization.vkInfo());
	}

	gpi.stageCount = u32(stages.size());
	gpi.pStages = stages.data();
	gpi.renderPass = src.renderPass->handle; // TODO: can this be destroyed?
	gpi.subpass = src.subpass;

	return vku::Pipeline(dev, gpi);
}

vku::Pipeline createPatchCopy(const RayTracingPipeline& src, VkShaderStageFlagBits stage,
		span<const u32> patchedSpv) {
	(void) src;
	(void) stage;
	(void) patchedSpv;
	dlg_error("unimplemented");
	return {};
}

vku::Pipeline createPatchCopy(const Pipeline& src, VkShaderStageFlagBits stage,
		span<const u32> patchedSpv) {
	if(src.type == VK_PIPELINE_BIND_POINT_COMPUTE) {
		return createPatchCopy(static_cast<const ComputePipeline&>(src), stage, patchedSpv);
	} else if(src.type == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		return createPatchCopy(static_cast<const GraphicsPipeline&>(src), stage, patchedSpv);
	} else if(src.type == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
		return createPatchCopy(static_cast<const RayTracingPipeline&>(src), stage, patchedSpv);
	} else {
		dlg_error("Unsupported pipe type!");
		return {};
	}
}

PatchJobResult patchJob(PatchJobData& data) {
	auto patchRes = patchShaderCapture(*data.pipe->dev, *data.compiler,
		data.file, data.line, data.captureAddress, data.entryPoint, data.stage);
	if(patchRes.copy.empty()) {
		PatchJobResult res {};
		res.error = "Shader patching failed";
		return res;
	}

	auto pipeName = name(*data.pipe, false);

#define VIL_OUTPUT_PATCHED_SPIRV
#ifdef VIL_OUTPUT_PATCHED_SPIRV
	std::string output = "vil";
	if(!pipeName.empty()) {
		output += "_";
		output += pipeName;
	}
	output += ".";

	auto hash = std::size_t(0u);
	for(auto v : patchRes.copy) hash_combine(hash, v);

	output += std::to_string(hash);
	output += ".spv";
	writeFile(output.c_str(), bytes(patchRes.copy), true);
#endif // VIL_OUTPUT_PATCHED_SPIRV

	auto pipe = createPatchCopy(*data.pipe, data.stage, patchRes.copy);
	if(!pipe.vkHandle()) {
		PatchJobResult res {};
		res.error = "Creating pipeline failed";
		return res;
	}

	auto newName = "patched:" + pipeName;
	nameHandle(pipe, newName);

	PatchJobResult res {};
	res.pipe = std::move(pipe);
	res.alloc = std::move(patchRes.alloc);
	res.captures = patchRes.captures;

	data.state = PatchJobState::done;

	return res;
}

} // namespace
