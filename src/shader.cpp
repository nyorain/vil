#include <shader.hpp>
#include <device.hpp>
#include <data.hpp>
#include <util/spirv.hpp>
#include <util/util.hpp>
#include <vk/enumString.hpp>

#include "spirv_reflect.h"

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

bool isOpInSection8(spv::Op op) {
	switch(op) {
		case spv::Op::OpDecorate:
		case spv::Op::OpMemberDecorate:
		case spv::Op::OpDecorationGroup:
		case spv::Op::OpGroupDecorate:
		case spv::Op::OpGroupMemberDecorate:
		case spv::Op::OpDecorateId:
		case spv::Op::OpDecorateString:
		case spv::Op::OpMemberDecorateString:
			return true;
		default:
			return false;
	}
}

bool isOpType(spv::Op op) {
	auto opu = unsigned(op);
	if(opu >= unsigned(spv::Op::OpTypeVoid) && opu <= unsigned(spv::Op::OpTypeForwardPointer)) {
		return true;
	}

	if(opu >= unsigned(spv::Op::OpTypeVmeImageINTEL) && opu <= unsigned(spv::Op::OpTypeAvcSicResultINTEL)) {
		return true;
	}

	switch(op) {
		case spv::Op::OpTypePipeStorage:
		case spv::Op::OpTypeNamedBarrier:
		case spv::Op::OpTypeRayQueryKHR:
		case spv::Op::OpTypeAccelerationStructureKHR:
		case spv::Op::OpTypeCooperativeMatrixNV:
			return true;
		default:
			break;
	}

	return false;
}

u32 typeSize(span<const u32> spirv, const std::unordered_map<u32, u32>& defs, u32 id) {
	auto it = defs.find(id);
	if(it == defs.end()) {
		dlg_error("Could not find type id {}", id);
		return u32(-1);
	}

	auto op = spv::Op(spirv[it->second] & 0xFFFFu);
	switch(op) {
		case spv::Op::OpTypeFloat:
		case spv::Op::OpTypeInt:
			return spirv[it->second + 2] / 32;
		case spv::Op::OpTypeArray:
		case spv::Op::OpTypeMatrix:
		case spv::Op::OpTypeVector: {
			auto nsize = typeSize(spirv, defs, spirv[it->second + 2]);
			if(nsize == u32(-1)) {
				return nsize;
			}

			auto count = spirv[it->second + 3];
			return count * nsize;
		} case spv::Op::OpTypeStruct: {
			auto wordCount = spirv[it->second] >> 16u;

			auto sum = 0u;
			for(auto i = 2u; i < wordCount; ++i) {
				auto nsize = typeSize(spirv, defs, spirv[it->second + i]);
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

	auto op = spv::Op(spirv[it->second] & 0xFFFFu);
	if(op != spv::Op::OpTypePointer) {
		dlg_error("Expected OpTypePointer");
		return u32(-1);
	}

	auto typeID = spirv[it->second + 3];
	return typeSize(spirv, defs, typeID);
}

XfbPatchData patchVertexShaderXfb(Device& dev, span<const u32> spirv, const char* entryPoint) {
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

	std::unordered_map<u32, u32> typedefs;
	typedefs.reserve(idBound);

	auto addedCap = false;
	auto addedExecutionMode = false;

	auto section = 0u;
	auto entryPointID = u32(-1);
	auto insertDecosPos = u32(-1);

	auto offset = 5u;
	while(offset < spirv.size()) {
		auto first = spirv[offset];
		auto op = spv::Op(first & 0xFFFFu);
		auto wordCount = first >> 16u;

		// We need to add the Xfb Execution mode to our entry point.
		if(section == 5u && op != spv::Op::OpEntryPoint) {
			dlg_assert_or(entryPointID != u32(-1), return {});

			section = 6u;
			patched.push_back((3u << 16) | u32(spv::Op::OpExecutionMode));
			patched.push_back(entryPointID);
			patched.push_back(u32(spv::ExecutionMode::Xfb));

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
		}

		// We need to add the TransformFeedback capability
		if(op == spv::Op::OpCapability) {
			dlg_assert(section <= 1u);
			section = 1u;

			dlg_assert(wordCount == 2);
			auto cap = spv::Capability(spirv[offset + 1]);

			// The shader *must* declare shader capability exactly once.
			// We add the transformFeedback cap just immediately after that.
			if(cap == spv::Capability::Shader) {
				dlg_assert(!addedCap);
				patched.push_back((2u << 16) | u32(spv::Op::OpCapability));
				patched.push_back(u32(spv::Capability::TransformFeedback));
				addedCap = true;
			}

			// When the shader itself declared that capability, there is
			// nothing we can do.
			// TODO: maybe in some cases shaders just declare that cap but
			// don't use it? In that case we could still patch in our own values
			if(cap == spv::Capability::TransformFeedback) {
				dlg_debug("Shader is already using transform feedback!");
				return {};
			}
		}

		// We need to find the id of the entry point.
		// We are also interested in the variables used by the entry point
		if(op == spv::Op::OpEntryPoint) {
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
		if(op == spv::Op::OpVariable) {
			dlg_assert(section <= 9u);
			section = 9u;

			dlg_assert_or(wordCount >= 4, return {});
			auto resType = spirv[offset + 1];
			auto resID = spirv[offset + 2];
			auto storage = spv::StorageClass(spirv[offset + 3]);
			if(storage == spv::StorageClass::Output && interfaceVars.count(resID)) {
				auto& cap = ret.desc->captures.emplace_back();
				cap.spirvVar = resID;
				cap.spirvPointerType = resType;
			}
		}

		if(op == spv::Op::OpDecorate) {
			dlg_assert(section <= 8u);
			section = 8u;

			dlg_assert(wordCount >= 3);
			auto resID = spirv[offset + 1];
			auto decoration = spv::Decoration(spirv[offset + 2]);
			if(decoration == spv::Decoration::BuiltIn) {
				dlg_assert(wordCount == 4);
				auto builtin = spv::BuiltIn(spirv[offset + 3]);

				// TODO: also capture the other builtin outputs?
				if(builtin == spv::BuiltIn::Position) {
					auto& cap = ret.desc->captures.emplace_back();
					cap.spirvVar = resID;
					cap.size = 16u; // vec4
				}
			}
		} else if(op == spv::Op::OpMemberDecorate) {
			dlg_assert(section <= 8u);
			section = 8u;

			dlg_assert(wordCount >= 4);
			auto structType = spirv[offset + 1];
			auto member = spirv[offset + 2];
			auto decoration = spv::Decoration(spirv[offset + 3]);
			if(decoration == spv::Decoration::BuiltIn) {
				dlg_assert(wordCount == 5);
				auto builtin = spv::BuiltIn(spirv[offset + 4]);

				// TODO: also capture the other builtin outputs?
				if(builtin == spv::BuiltIn::Position) {
					auto& cap = ret.desc->captures.emplace_back();
					cap.structType = structType;
					cap.member = member;
					cap.size = 16u; // vec4
				}
			}
		}

		// need to store locations of type definitions
		if(isOpType(op)) {
			dlg_assert(section <= 9u);
			section = 9u;

			auto resID = spirv[offset + 1];
			auto [_, success] = typedefs.emplace(resID, offset);
			dlg_assert(success);
		}

		offset += wordCount;
	}

	if(!addedCap || !addedExecutionMode || ret.desc->captures.empty() || insertDecosPos == u32(-1)) {
		dlg_warn("Could not inject xfb into shader. Likely error inside vil. "
			"capability: {}, executionMode: {}, captures.size(): {}, decosPos: {}",
			addedCap, addedExecutionMode, ret.desc->captures.size(), insertDecosPos);
		return {};
	}

	// parse sizes
	auto bufOffset = 0u;
	for(auto& cap : ret.desc->captures) {
		if(!cap.size) {
			dlg_assert(cap.spirvVar != u32(-1) && cap.spirvPointerType != u32(-1));
			cap.size = pointerTypeSize(spirv, typedefs, cap.spirvPointerType);
			if(cap.size == u32(-1)) {
				dlg_warn("Cannot determine output variable size; Aborting xfb injection");
				return {};
			}
		}

		cap.offset = bufOffset;
		bufOffset += cap.size;
	}

	// generate decos
	std::vector<u32> newDecos;

	auto addDeco = [&](u32 target, spv::Decoration deco, u32 value) {
		newDecos.push_back((4u << 16) | u32(spv::Op::OpDecorate));
		newDecos.push_back(target);
		newDecos.push_back(u32(deco));
		newDecos.push_back(value);
	};

	auto addMemberDeco = [&](u32 structType, u32 member, spv::Decoration deco, u32 value) {
		newDecos.push_back((5u << 16) | u32(spv::Op::OpMemberDecorate));
		newDecos.push_back(structType);
		newDecos.push_back(member);
		newDecos.push_back(u32(deco));
		newDecos.push_back(value);
	};

	// PERF: stride align 8 is only needed when we have double vars,
	// otherwise 4 would be enough. Track that somehow.
	ret.desc->stride = align(bufOffset, 8u);

	for(auto& cap : ret.desc->captures) {
		if(cap.spirvVar != u32(-1)) {
			addDeco(cap.spirvVar, spv::Decoration::XfbBuffer, 0u);
			addDeco(cap.spirvVar, spv::Decoration::XfbStride, ret.desc->stride);
			addDeco(cap.spirvVar, spv::Decoration::Offset, cap.offset);
		} else {
			dlg_assert(cap.structType != u32(-1) && cap.member != u32(-1));
			addMemberDeco(cap.structType, cap.member, spv::Decoration::XfbBuffer, 0u);
			addMemberDeco(cap.structType, cap.member, spv::Decoration::XfbStride, ret.desc->stride);
			addMemberDeco(cap.structType, cap.member, spv::Decoration::Offset, cap.offset);
		}
	}

	// insert decos into patched spirv
	patched.insert(patched.begin() + insertDecosPos, newDecos.begin(), newDecos.end());

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
