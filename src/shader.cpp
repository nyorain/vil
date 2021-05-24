#include <shader.hpp>
#include <device.hpp>
#include <data.hpp>
#include <util/spirv.hpp>
#include <util/util.hpp>
#include <vk/enumString.hpp>
#include <util/spirv_reflect.h>

#define SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS
#include <spirv-cross/spirv_cross.hpp>
namespace spc = spirv_cross;

namespace vil {

// util
std::string extractString(span<const u32> spirv) {
	std::string ret;
	for(auto w : spirv) {
		for (auto j = 0u; j < 4; j++, w >>= 8) {
			char c = w & 0xff;
			if(c == '\0') {
				return ret;
			}

			ret += c;
		}
	}

	dlg_error("Unterminated SPIR-V string");
	return {};
}

bool isOpInSection8(spv11::Op op) {
	switch(op) {
		case spv11::Op::OpDecorate:
		case spv11::Op::OpMemberDecorate:
		case spv11::Op::OpDecorationGroup:
		case spv11::Op::OpGroupDecorate:
		case spv11::Op::OpGroupMemberDecorate:
		case spv11::Op::OpDecorateId:
		case spv11::Op::OpDecorateString:
		case spv11::Op::OpMemberDecorateString:
			return true;
		default:
			return false;
	}
}

bool isOpType(spv11::Op op) {
	auto opu = unsigned(op);
	if(opu >= unsigned(spv11::Op::OpTypeVoid) && opu <= unsigned(spv11::Op::OpTypeForwardPointer)) {
		return true;
	}

	if(opu >= unsigned(spv11::Op::OpTypeVmeImageINTEL) && opu <= unsigned(spv11::Op::OpTypeAvcSicResultINTEL)) {
		return true;
	}

	switch(op) {
		case spv11::Op::OpTypePipeStorage:
		case spv11::Op::OpTypeNamedBarrier:
		case spv11::Op::OpTypeRayQueryKHR:
		case spv11::Op::OpTypeAccelerationStructureKHR:
		case spv11::Op::OpTypeCooperativeMatrixNV:
			return true;
		default:
			break;
	}

	return false;
}

bool isOpConstant(spv11::Op op) {
	switch(op) {
		case spv11::Op::OpConstantTrue:
		case spv11::Op::OpConstantFalse:
		case spv11::Op::OpConstant:
		case spv11::Op::OpConstantComposite:
		case spv11::Op::OpConstantSampler:
		case spv11::Op::OpConstantNull:
		case spv11::Op::OpConstantPipeStorage:

		case spv11::Op::OpSpecConstantTrue:
		case spv11::Op::OpSpecConstantFalse:
		case spv11::Op::OpSpecConstant:
		case spv11::Op::OpSpecConstantComposite:
		case spv11::Op::OpSpecConstantOp:
			return true;

		default:
			return false;
	}
}

u32 typeSize(span<const u32> spirv, const std::unordered_map<u32, u32>& locs, u32 id) {
	auto it = locs.find(id);
	if(it == locs.end()) {
		dlg_error("Could not find type id {}", id);
		return u32(-1);
	}

	auto op = spv11::Op(spirv[it->second] & 0xFFFFu);
	auto wordCount = spirv[it->second] >> 16u;

	switch(op) {
		case spv11::Op::OpTypeFloat:
		case spv11::Op::OpTypeInt:
			dlg_assert(wordCount >= 3);
			return spirv[it->second + 2] / 8;
		case spv11::Op::OpTypeArray:
		case spv11::Op::OpTypeMatrix:
		case spv11::Op::OpTypeVector: {
			dlg_assert(wordCount == 4);
			auto nsize = typeSize(spirv, locs, spirv[it->second + 2]);
			if(nsize == u32(-1)) {
				return nsize;
			}

			auto count = spirv[it->second + 3];
			return count * nsize;
		} case spv11::Op::OpTypeStruct: {
			auto sum = 0u;
			for(auto i = 2u; i < wordCount; ++i) {
				auto nsize = typeSize(spirv, locs, spirv[it->second + i]);
				if(nsize == u32(-1)) {
					return nsize;
				}

				sum += nsize;
			}

			return sum;
		} default:
			dlg_error("typeSize: Unhandled op");
			return u32(-1);
	}
}

u32 pointerTypeSize(span<const u32> spirv, const std::unordered_map<u32, u32>& defs,
		u32 pointerID) {
	auto it = defs.find(pointerID);
	if(it == defs.end()) {
		dlg_error("Could not find initial pointer id {}", pointerID);
		return u32(-1);
	}

	auto op = spv11::Op(spirv[it->second] & 0xFFFFu);
	if(op != spv11::Op::OpTypePointer) {
		dlg_error("Expected OpTypePointer");
		return u32(-1);
	}

	auto typeID = spirv[it->second + 3];
	return typeSize(spirv, defs, typeID);
}

u32 baseTypeSize(const spc::SPIRType& type, XfbCapture& cap) {
	dlg_assert(!type.pointer);

	switch(type.basetype) {
		case spc::SPIRType::Float:
		case spc::SPIRType::Half:
		case spc::SPIRType::Double:
			cap.type = XfbCapture::typeFloat;
			break;

		case spc::SPIRType::UInt:
		case spc::SPIRType::UInt64:
		case spc::SPIRType::UByte:
		case spc::SPIRType::UShort:
			cap.type = XfbCapture::typeUint;
			break;

		case spc::SPIRType::Int:
		case spc::SPIRType::Int64:
		case spc::SPIRType::SByte:
		case spc::SPIRType::Short:
			cap.type = XfbCapture::typeInt;
			break;

		default:
			dlg_assert("Invalid type");
			return u32(-1);
	}

	cap.width = type.width;
	cap.columns = type.columns;
	cap.vecsize = type.vecsize;

	return type.vecsize * type.columns * (type.width / 8);
}

void addDeco(std::vector<u32>& newDecos, u32 target, spv11::Decoration deco, u32 value) {
	newDecos.push_back((4u << 16) | u32(spv11::Op::OpDecorate));
	newDecos.push_back(target);
	newDecos.push_back(u32(deco));
	newDecos.push_back(value);
}

void addMemberDeco(std::vector<u32>& newDecos, u32 structType, u32 member, spv11::Decoration deco, u32 value) {
	newDecos.push_back((5u << 16) | u32(spv11::Op::OpMemberDecorate));
	newDecos.push_back(structType);
	newDecos.push_back(member);
	newDecos.push_back(u32(deco));
	newDecos.push_back(value);
}

void annotateCapture(const spc::Compiler& compiler, const spc::SPIRType& structType,
		const std::string& name, u32& bufOffset, std::vector<XfbCapture>& captures,
		std::vector<u32>& newDecos) {
	for(auto i = 0u; i < structType.member_types.size(); ++i) {
		auto& member = structType.member_types[i];
		auto& mtype = compiler.get_type(member);

		auto memberName = name;
		auto mname = compiler.get_member_name(structType.self, i);
		if(!mname.empty()) {
			memberName += mname;
		} else {
			memberName += std::to_string(i);
		}

		if(mtype.basetype == spc::SPIRType::Struct) {
			memberName += ".";
			annotateCapture(compiler, mtype, memberName, bufOffset, captures, newDecos);
			continue;
		}

		XfbCapture cap {};
		auto baseSize = baseTypeSize(mtype, cap);
		if(baseSize == u32(-1)) {
			continue;
		}

		auto size = baseSize;
		for(auto j = 0u; j < mtype.array.size(); ++j) {
			auto dim = mtype.array[j];
			if(!mtype.array_size_literal[j]) {
				dim = compiler.evaluate_constant_u32(dim);
			}

			cap.array.push_back(dim);
			size *= dim;
		}

		if(!compiler.has_member_decoration(structType.self, i, spv::DecorationArrayStride) &&
				!mtype.array.empty()) {
			// compiler.set_member_decoration(structType.self, i, spv::DecorationArrayStride, baseSize);
			addMemberDeco(newDecos, structType.self, i, spv11::Decoration::ArrayStride, baseSize);
		}

		dlg_assert_or(!compiler.has_member_decoration(structType.self, i, spv::DecorationOffset), continue);
		// TODO: have to align offset properly for 64-bit types
		// compiler.set_member_decoration(structType.self, i, spv::DecorationOffset, bufOffset);
		addMemberDeco(newDecos, structType.self, i, spv11::Decoration::Offset, bufOffset);

		if(compiler.has_member_decoration(structType.self, i, spv::DecorationBuiltIn)) {
			cap.builtin = compiler.get_member_decoration(structType.self, i, spv::DecorationBuiltIn);
		}

		cap.name = memberName;
		cap.offset = bufOffset;

		captures.push_back(std::move(cap));
		bufOffset += size;
	}
}

// TODO: consider specialization constants! E.g. important for array sizes
XfbPatchData patchVertexShaderXfb(Device& dev, span<const u32> spirv,
		const char* entryPoint, std::string_view modName) {
	ZoneScoped;

	// parse spirv
	if(spirv.size() < 5) {
		dlg_error("spirv to small");
		return {};
	}

	if(spirv[0] != 0x07230203) {
		dlg_error("Invalid spirv magic number. Endianess troubles?");
		return {};
	}

	XfbPatchData ret {};
	ret.desc.reset(new XfbPatchDesc());

	// auto version = spirv[1];
	// auto generator = spirv[2];
	auto idBound = spirv[3];

	std::unordered_set<u32> interfaceVars;
	std::vector<u32> patched {spirv.begin(), spirv.begin() + 5};
	patched.reserve(spirv.size());

	std::unordered_map<u32, u32> locs;
	locs.reserve(idBound);

	auto addedCap = false;
	auto addedExecutionMode = false;

	auto section = 0u;
	auto entryPointID = u32(-1);
	auto insertDecosPos = u32(-1);

	std::vector<u32> captureVars;

	auto offset = 5u;
	u32 badHash = 0u;
	while(offset < spirv.size()) {
		auto first = spirv[offset];
		auto op = spv11::Op(first & 0xFFFFu);
		auto wordCount = first >> 16u;

		// We need to add the Xfb Execution mode to our entry point.
		if(section == 5u && op != spv11::Op::OpEntryPoint) {
			dlg_assert_or(entryPointID != u32(-1), return {});

			section = 6u;
			patched.push_back((3u << 16) | u32(spv11::Op::OpExecutionMode));
			patched.push_back(entryPointID);
			patched.push_back(u32(spv11::ExecutionMode::Xfb));

			addedExecutionMode = true;
		}

		// check if we have reached section 8
		if(isOpInSection8(op) && insertDecosPos == u32(-1)) {
			dlg_assert(section < 8);
			section = 8u;
			insertDecosPos = u32(patched.size());
		}

		for(auto i = 0u; i < wordCount; ++i) {
			patched.push_back(spirv[offset + i]);
			badHash ^= spirv[offset + i];
		}

		// We need to add the TransformFeedback capability
		if(op == spv11::Op::OpCapability) {
			dlg_assert(section <= 1u);
			section = 1u;

			dlg_assert(wordCount == 2);
			auto cap = spv11::Capability(spirv[offset + 1]);

			// The shader *must* declare shader capability exactly once.
			// We add the transformFeedback cap just immediately after that.
			if(cap == spv11::Capability::Shader) {
				dlg_assert(!addedCap);
				patched.push_back((2u << 16) | u32(spv11::Op::OpCapability));
				patched.push_back(u32(spv11::Capability::TransformFeedback));
				addedCap = true;
			}

			// When the shader itself declared that capability, there is
			// nothing we can do.
			// TODO: maybe in some cases shaders just declare that cap but
			// don't use it? In that case we could still patch in our own values
			if(cap == spv11::Capability::TransformFeedback) {
				dlg_debug("Shader is already using transform feedback!");
				return {};
			}
		}

		// We need to find the id of the entry point.
		// We are also interested in the variables used by the entry point
		if(op == spv11::Op::OpEntryPoint) {
			dlg_assert(section <= 5u);
			section = 5u;

			dlg_assert(wordCount >= 4);
			auto length = wordCount - 3;
			auto name = extractString(span(spirv).subspan(offset + 3, length));
			if(!name.empty() && name == entryPoint) {
				dlg_assert(entryPointID == u32(-1));
				entryPointID = spirv[offset + 2];

				dlg_assert(interfaceVars.empty());
				auto interfaceOffset = 3 + ceilDivide(name.size() + 1, 4);
				dlg_assert(interfaceOffset <= wordCount);
				for(auto i = interfaceOffset; i < wordCount; ++i) {
					auto id = spirv[offset + i];
					dlg_assert(id < idBound);
					interfaceVars.emplace(id);
				}
			}
		}

		// We need to add our xfb decorations to outputs from the shader stage
		if(op == spv11::Op::OpVariable) {
			dlg_assert(section <= 9u);
			section = 9u;

			dlg_assert_or(wordCount >= 4, return {});
			// auto resType = spirv[offset + 1];
			auto resID = spirv[offset + 2];
			auto storage = spv11::StorageClass(spirv[offset + 3]);
			if(storage == spv11::StorageClass::Output && interfaceVars.count(resID)) {
				// auto& cap = ret.desc->captures.emplace_back();
				captureVars.push_back(resID);
				// cap.spirvVar = resID;
				// cap.spirvPointerType = resType;
			}
		}

		// need to store locations of type definitions
		if(isOpType(op)) {
			dlg_assert(section <= 9u);
			section = 9u;

			auto resID = spirv[offset + 1];
			auto [_, success] = locs.emplace(resID, offset);
			dlg_assert(success);
		}

		offset += wordCount;
	}

	if(!addedCap || !addedExecutionMode || captureVars.empty() || insertDecosPos == u32(-1)) {
		dlg_warn("Could not inject xfb into shader. Likely error inside vil. "
			"capability: {}, executionMode: {}, captureVars.size(): {}, decosPos: {}",
			addedCap, addedExecutionMode, captureVars.size(), insertDecosPos);
		return {};
	}

	// parse sizes, build the vector of captured output values.
	spc::Compiler compiler(std::vector<u32>(spirv.begin(), spirv.end()));
	compiler.set_entry_point(entryPoint, spv::ExecutionModelVertex);
	compiler.compile();

	std::vector<u32> newDecos;

	auto bufOffset = 0u;
	for(auto& var : captureVars) {
		auto& ptype = compiler.get_type_from_variable(var);
		dlg_assert(ptype.pointer);
		dlg_assert(ptype.parent_type);
		auto& type = compiler.get_type(ptype.parent_type);

		auto name = compiler.get_name(var);
		if(name.empty()) {
			name = dlg::format("Output{}", var);
		}

		if(type.basetype == spc::SPIRType::Struct) {
			name += ".";
			annotateCapture(compiler, type, name, bufOffset, ret.desc->captures, newDecos);
		} else {
			XfbCapture cap {};
			auto baseSize = baseTypeSize(type, cap);
			if(baseSize == u32(-1)) {
				continue;
			}

			auto size = baseSize;
			for(auto j = 0u; j < type.array.size(); ++j) {
				auto dim = type.array[j];
				if(!type.array_size_literal[j]) {
					dim = compiler.evaluate_constant_u32(dim);
				}

				cap.array.push_back(dim);
				size *= dim;
			}

			if(!compiler.has_decoration(type.self, spv::DecorationArrayStride) &&
					!type.array.empty()) {
				// compiler.set_decoration(type.self, spv::DecorationArrayStride, baseSize);
				addDeco(newDecos, var, spv11::Decoration::ArrayStride, baseSize);
			}

			dlg_assert_or(!compiler.has_decoration(var, spv::DecorationOffset), continue);
			// TODO: have to align offset properly for 64-bit types
			// compiler.set_decoration(var, spv::DecorationOffset, bufOffset);
			addDeco(newDecos, var, spv11::Decoration::Offset, bufOffset);

			if(compiler.has_decoration(var, spv::DecorationBuiltIn)) {
				cap.builtin = compiler.get_decoration(var, spv::DecorationBuiltIn);
			}

			cap.name = name;
			cap.offset = bufOffset;

			ret.desc->captures.push_back(std::move(cap));
			bufOffset += size;
		}
	}

	// PERF: stride align 8 is only needed when we have double vars,
	// otherwise 4 would be enough. Track that somehow.
	ret.desc->stride = align(bufOffset, 8u);

	for(auto& var : captureVars) {
		addDeco(newDecos, var, spv11::Decoration::XfbBuffer, 0u);
		addDeco(newDecos, var, spv11::Decoration::XfbStride, ret.desc->stride);

		// compiler.set_decoration(var, spv::DecorationXfbBuffer, 0u);
		// compiler.set_decoration(var, spv::DecorationXfbStride, ret.desc->stride);
	}

	// insert decos into patched spirv
	patched.insert(patched.begin() + insertDecosPos, newDecos.begin(), newDecos.end());

	// TODO: tmp
	std::string output = "vil";
	if(!modName.empty()) {
		output += "_";
		output += modName;
	}
	output += ".";
	output += std::to_string(badHash);
	output += ".spv";
	writeFile(output.c_str(), bytes(patched), true);

	dlg_info("xfb: {}, stride {}", output, ret.desc->stride);
	for(auto& cap : ret.desc->captures) {
		dlg_info("  {}", cap.name);
		dlg_info("  >> offset {}", cap.offset);
		dlg_info("  >> size {}", cap.width * cap.columns * cap.vecsize);
		if(cap.builtin) {
			dlg_info("  >> builtin {}", *cap.builtin);
		}
	}

	VkShaderModuleCreateInfo ci {};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.pCode = patched.data();
	ci.codeSize = patched.size() * 4;

	auto res = dev.dispatch.CreateShaderModule(dev.handle, &ci, nullptr, &ret.mod);
	if(res != VK_SUCCESS) {
		dlg_error("xfb CreateShaderModule: {} (res)", vk::name(res), res);
		return {};
	}

	ret.entryPoint = entryPoint;
	return ret;
}


// ShaderModule
SpirvData::~SpirvData() {
	if(reflection.get()) {
		spvReflectDestroyShaderModule(reflection.get());
	}
}

ShaderModule::~ShaderModule() {
	if(!dev) {
		return;
	}

	for(auto& patched : this->xfb) {
		dev->dispatch.DestroyShaderModule(dev->handle, patched.mod, nullptr);
	}
}

// api
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(
		VkDevice                                    device,
		const VkShaderModuleCreateInfo*             pCreateInfo,
		const VkAllocationCallbacks*                pAllocator,
		VkShaderModule*                             pShaderModule) {
	auto& dev = getDevice(device);
	auto res = dev.dispatch.CreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
	if(res != VK_SUCCESS) {
		return res;
	}

	auto& mod = dev.shaderModules.add(*pShaderModule);
	mod.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
	mod.dev = &dev;
	mod.handle = *pShaderModule;

	dlg_assert(pCreateInfo->codeSize % 4 == 0);
	mod.spv = {pCreateInfo->pCode, pCreateInfo->pCode + pCreateInfo->codeSize / 4};

	mod.code = IntrusivePtr<SpirvData>(new SpirvData());
	mod.code->reflection = std::make_unique<SpvReflectShaderModule>();
	auto reflRes = spvReflectCreateShaderModule(pCreateInfo->codeSize,
		pCreateInfo->pCode, mod.code->reflection.get());
	dlg_assertl(dlg_level_info, reflRes == SPV_REFLECT_RESULT_SUCCESS);

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
		VkDevice                                    device,
		VkShaderModule                              shaderModule,
		const VkAllocationCallbacks*                pAllocator) {
	if (!shaderModule) {
		return;
	}

	auto& dev = getDevice(device);
	dev.shaderModules.mustErase(shaderModule);
	dev.dispatch.DestroyShaderModule(device, shaderModule, pAllocator);
}

} // namespace vil
