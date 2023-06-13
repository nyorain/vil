#include <shader.hpp>
#include <device.hpp>
#include <wrap.hpp>
#include <util/spirv.hpp>
#include <util/util.hpp>
#include <vkutil/enumString.hpp>
#include <spirv-cross/spirv_cross.hpp>
#include <filesystem>
#include <optional>

// NOTE: useful for debugging of patching issues, not enabled by default.
// #define VIL_OUTPUT_PATCHED_SPIRV

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

ShaderSpecialization createShaderSpecialization(const VkSpecializationInfo* info) {
	ShaderSpecialization ret;
	if(!info) {
		return ret;
	}

	auto data = static_cast<const std::byte*>(info->pData);
	ret.entries = {info->pMapEntries, info->pMapEntries + info->mapEntryCount};
	ret.data = {data, data + info->dataSize};
	return ret;
}

bool operator==(const ShaderSpecialization& a, const ShaderSpecialization& b) {
	if(a.entries.size() != b.entries.size()) {
		return false;
	}

	// since they have the same number of entries, for equality we only
	// have to show that each entry in a has an equivalent in b.
	for(auto& ea : a.entries) {
		auto found = false;
		for(auto& eb : b.entries) {
			if(ea.constantID != eb.constantID) {
				continue;
			}

			if(ea.size == eb.size) {
				dlg_assert(ea.offset + ea.size <= a.data.size());
				dlg_assert(eb.offset + eb.size <= b.data.size());

				// NOTE: keep in mind this might be more strict than
				// an equality check e.g. for floating point NaNs. But that
				// shouldn't be a problem for any use of this function.
				auto cmp = std::memcmp(
					a.data.data() + ea.offset,
					b.data.data() + eb.offset,
					ea.size);
				found = (cmp == 0u);
			}

			break;
		}

		if(!found) {
			return false;
		}
	}

	return true;
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
		} else if(!name.empty()) {
			// only append if there is already something useful in the name
			memberName += std::to_string(i);
		}

		if(mtype.basetype == spc::SPIRType::Struct) {
			if(!memberName.empty()) {
				memberName += ".";
			}

			annotateCapture(compiler, mtype, memberName, bufOffset, captures, newDecos);
			continue;
		}

		XfbCapture cap {};
		auto baseSize = baseTypeSize(mtype, cap);
		if(baseSize == u32(-1)) {
			continue;
		}

		if(compiler.has_member_decoration(structType.self, i, spv::DecorationBuiltIn)) {
			// filter out unwritten builtins
			auto builtin = compiler.get_member_decoration(structType.self, i, spv::DecorationBuiltIn);
			if(!compiler.has_active_builtin(spv::BuiltIn(builtin), spv::StorageClassOutput)) {
				continue;
			}
			cap.builtin = builtin;
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

		// if(!compiler.has_decoration(mtype.self, spv::DecorationArrayStride) &&
		// 		!mtype.array.empty()) {
		// 	addDeco(newDecos, mtype.self, spv11::Decoration::ArrayStride, baseSize);
		// }

		dlg_assert_or(!compiler.has_member_decoration(structType.self, i, spv::DecorationOffset), continue);
		// TODO: have to align offset properly for 64-bit types
		addMemberDeco(newDecos, structType.self, i, spv11::Decoration::Offset, bufOffset);

		cap.offset = bufOffset;
		cap.name = memberName;

		captures.push_back(std::move(cap));
		bufOffset += size;
	}
}

XfbPatchRes patchSpirvXfb(spc::Compiler& compiled, const char* entryPoint) {
	auto spirv = span<const u32>(compiled.get_ir().spirv);

	// parse spirv
	if(spirv.size() < 5) {
		dlg_error("spirv to small");
		return {};
	}

	if(spirv[0] != 0x07230203) {
		dlg_error("Invalid spirv magic number. Endianess troubles?");
		return {};
	}

	std::vector<u32> patched;
	patched.reserve(spirv.size());
	patched.resize(5);
	std::copy(spirv.begin(), spirv.begin() + 5, patched.begin());

	auto addedCap = false;
	auto addedExecutionMode = false;

	auto section = 0u;
	auto entryPointID = u32(-1);
	auto insertDecosPos = u32(-1);

	auto offset = 5u;
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
			}
		}

		offset += wordCount;
	}

	if(!addedCap || !addedExecutionMode || insertDecosPos == u32(-1)) {
		dlg_warn("Could not inject xfb into shader. Likely error inside vil. "
			"capability: {}, executionMode: {}, captureVars.size(): {}, decosPos: {}",
			addedCap, addedExecutionMode, insertDecosPos);
		return {};
	}

	// parse sizes, build the vector of captured output values.
	compiled.update_active_builtins();

	std::vector<XfbCapture> captures;
	std::vector<u32> newDecos;

	auto ivars = compiled.get_active_interface_variables();

	auto bufOffset = 0u;
	for(auto& var : ivars) {
		auto storage = compiled.get_storage_class(var);
		if(storage != spv::StorageClassOutput) {
			continue;
		}

		auto& ptype = compiled.get_type_from_variable(var);
		dlg_assert(ptype.pointer);
		dlg_assert(ptype.parent_type);
		auto& type = compiled.get_type(ptype.parent_type);

		auto name = compiled.get_name(var); // might be empty
		if(type.basetype == spc::SPIRType::Struct) {
			if(!name.empty()) {
				name += ".";
			}

			annotateCapture(compiled, type, name, bufOffset, captures, newDecos);
		} else {
			XfbCapture cap {};
			auto baseSize = baseTypeSize(type, cap);
			if(baseSize == u32(-1)) {
				continue;
			}

			if(compiled.has_decoration(var, spv::DecorationBuiltIn)) {
				// filter out unwritten builtins
				auto builtin = compiled.get_decoration(var, spv::DecorationBuiltIn);
				if(!compiled.has_active_builtin(spv::BuiltIn(builtin), spv::StorageClassOutput)) {
					continue;
				}
				cap.builtin = builtin;
			}

			auto size = baseSize;
			for(auto j = 0u; j < type.array.size(); ++j) {
				auto dim = type.array[j];
				if(!type.array_size_literal[j]) {
					dim = compiled.evaluate_constant_u32(dim);
				}

				cap.array.push_back(dim);
				size *= dim;
			}

			// if(!compiled.has_decoration(type.self, spv::DecorationArrayStride) &&
			// 		!type.array.empty()) {
			// 	addDeco(newDecos, type.self, spv11::Decoration::ArrayStride, baseSize);
			// }

			dlg_assert_or(!compiled.has_decoration(var, spv::DecorationOffset), continue);
			// TODO: have to align offset properly for 64-bit types
			// compiler.set_decoration(var, spv::DecorationOffset, bufOffset);
			addDeco(newDecos, var, spv11::Decoration::Offset, bufOffset);

			cap.offset = bufOffset;
			cap.name = name;

			captures.push_back(std::move(cap));
			bufOffset += size;
		}
	}

	if(captures.empty()) {
		dlg_info("xfb: nothing to capture?! Likely a vil error");
		return {};
	}

	// TODO: stride align 8 is only needed when we have double vars,
	// otherwise 4 would be enough. Track that somehow. The same way
	// we'd also have to align 64-bit types, see above.
	// auto stride = align(bufOffset, 8u);
	auto stride = bufOffset;

	for(auto& var : ivars) {
		auto storage = compiled.get_storage_class(var);
		if(storage != spv::StorageClassOutput) {
			continue;
		}

		addDeco(newDecos, var, spv11::Decoration::XfbBuffer, 0u);
		addDeco(newDecos, var, spv11::Decoration::XfbStride, stride);
	}

	// insert decos into patched spirv
	patched.insert(patched.begin() + insertDecosPos, newDecos.begin(), newDecos.end());

	auto desc = IntrusivePtr<XfbPatchDesc>(new XfbPatchDesc());
	desc->captures = std::move(captures);
	desc->stride = stride;
	return {patched, std::move(desc)};
}

XfbPatchData patchShaderXfb(Device& dev, spc::Compiler& compiled,
		const char* entryPoint, std::string_view modName) {
	ZoneScoped;

	auto patched = patchSpirvXfb(compiled, entryPoint);
	if(!patched.desc) {
		return {};
	}

	(void) modName;

// #define VIL_OUTPUT_PATCHED_SPIRV
#ifdef VIL_OUTPUT_PATCHED_SPIRV
	std::string output = "vil";
	if(!modName.empty()) {
		output += "_";
		output += modName;
	}
	output += ".";

	auto badHash = u32(0u);
	for(auto v : patched.spirv) badHash ^= v;

	output += std::to_string(badHash);
	output += ".spv";
	writeFile(output.c_str(), bytes(patched.spirv), true);

	dlg_info("xfb: {}, stride {}", output, patched.desc->stride);
	dlg_info("cwd: {}", std::filesystem::current_path());
	for(auto& cap : patched.desc->captures) {
		dlg_info("  {}", cap.name);
		dlg_info("  >> offset {}", cap.offset);
		dlg_info("  >> size {}", (cap.width * cap.columns * cap.vecsize) / 8);
		if(cap.builtin) {
			dlg_info("  >> builtin {}", *cap.builtin);
		}
	}
#endif // VIL_OUTPUT_PATCHED_SPIRV

	VkShaderModuleCreateInfo ci {};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.pCode = patched.spirv.data();
	ci.codeSize = patched.spirv.size() * 4;

	XfbPatchData ret;
	auto res = dev.dispatch.CreateShaderModule(dev.handle, &ci, nullptr, &ret.mod);
	if(res != VK_SUCCESS) {
		dlg_error("xfb CreateShaderModule: {} (res)", vk::name(res), res);
		return {};
	}

	std::string pname = std::string(modName);
	pname += "(vil:xfb-patched)";
	nameHandle(dev, ret.mod, pname.c_str());

	ret.entryPoint = entryPoint;
	ret.desc = std::move(patched.desc);
	return ret;
}


// ShaderModule
ShaderModule::ShaderModule() = default;

ShaderModule::~ShaderModule() {
	if(!dev) {
		return;
	}

	// clearXfb should have been called on API object destruction
	dlg_assert(xfb.empty());
}

void ShaderModule::clearXfb() {
	for(auto& patched : this->xfb) {
		dev->dispatch.DestroyShaderModule(dev->handle, patched.mod, nullptr);
	}
	xfb.clear();
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
	mod.dev = &dev;
	mod.handle = *pShaderModule;

	dlg_assert(pCreateInfo->codeSize % 4 == 0);

	// TODO: catch errors here
	mod.compiled = std::make_unique<spc::Compiler>(
		pCreateInfo->pCode, pCreateInfo->codeSize / 4);

	// compute hash
	for(auto i = 0u; i < pCreateInfo->codeSize / 4; ++i) {
		hash_combine(mod.spirvHash, pCreateInfo->pCode[i]);
	}

	// copy default values of specialization constants
	auto& compiled = *mod.compiled;
	auto specConstants = compiled.get_specialization_constants();
	for(auto& sc : specConstants) {
		auto& entry = mod.constantDefaults.emplace_back();
		entry.constantID = sc.constant_id;

		auto& constant = compiled.get_constant(sc.id);
		dlg_assert(constant.m.columns == 1u);
		dlg_assert(constant.m.c[0].vecsize == 1u);
		entry.constant = std::make_unique<spc::SPIRConstant>(constant);
	}

	return res;
}

VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(
		VkDevice                                    device,
		VkShaderModule                              shaderModule,
		const VkAllocationCallbacks*                pAllocator) {
	if (!shaderModule) {
		return;
	}

	auto mod = mustMoveUnset(device, shaderModule);
	// memory optimization:
	// we don't need any xfb-patched data anymore
	mod->clearXfb();
	mod->dev->dispatch.DestroyShaderModule(device, shaderModule, pAllocator);
}

std::optional<spc::Resource> resource(const spc::Compiler& compiler,
		u32 setID, u32 bindingID, VkDescriptorType type) {
	std::optional<spc::Resource> ret;

	auto check = [&](auto& resources, span<const VkDescriptorType> trefs) {
		if(type != VK_DESCRIPTOR_TYPE_MAX_ENUM && !contains(trefs, type)) {
			return;
		}

		for(auto& res : resources) {
			if(!compiler.has_decoration(res.id, spv::DecorationDescriptorSet) ||
					!compiler.has_decoration(res.id, spv::DecorationBinding)) {
				dlg_warn("resource {} doesn't have set/binding decorations", res.name);
				continue;
			}

			auto sid = compiler.get_decoration(res.id, spv::DecorationDescriptorSet);
			auto bid = compiler.get_decoration(res.id, spv::DecorationBinding);

			if(sid == setID && bid == bindingID) {
				dlg_assert(!ret);
				ret = res;
				break;
			}
		}
	};

	auto resources = compiler.get_shader_resources();
	check(resources.acceleration_structures, {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}});
	check(resources.sampled_images, {{
		VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
	}});
	check(resources.separate_images, {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE}});
	check(resources.separate_samplers, {{VK_DESCRIPTOR_TYPE_SAMPLER}});
	check(resources.storage_images, {{
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
	}});
	check(resources.storage_buffers, {{
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
	}});
	check(resources.uniform_buffers, {{
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
		VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT,
	}});
	check(resources.subpass_inputs, {{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT}});

	return ret;
}

std::optional<spc::Resource> resource(const spc::Compiler& compiler, u32 varID) {
	std::optional<spc::Resource> ret;

	auto check = [&](auto& resources) {
		for(auto& res : resources) {
			if(res.id == varID) {
				dlg_assert(!ret);
				ret = res;
				break;
			}
		}
	};

	auto resources = compiler.get_shader_resources();
	check(resources.acceleration_structures);
	check(resources.sampled_images);
	check(resources.separate_images);
	check(resources.separate_samplers);
	check(resources.storage_buffers);
	check(resources.storage_images);
	check(resources.uniform_buffers);
	check(resources.subpass_inputs);

	return ret;
}

std::optional<spc::BuiltInResource> builtinResource(const spc::Compiler& compiler, u32 varID) {
	std::optional<spc::BuiltInResource> ret;

	auto checkBuiltin = [&](auto& resources) {
		for(auto& res : resources) {
			if(res.resource.id == varID) {
				dlg_assert(!ret);
				ret = res;
				break;
			}
		}
	};

	auto resources = compiler.get_shader_resources();
	checkBuiltin(resources.builtin_inputs);
	checkBuiltin(resources.builtin_outputs);

	return ret;
}

BindingNameRes bindingName(const spc::Compiler& compiler, u32 setID, u32 bindingID) {
	auto ores = resource(compiler, setID, bindingID);
	if(!ores) {
		return {BindingNameRes::Type::notfound, {}};
	}

	auto& res = *ores;
	if(res.name.empty()) {
		return {BindingNameRes::Type::unnamed, {}};
	}

	return {BindingNameRes::Type::valid, std::move(res.name)};
}

void specializeSpirv(spc::Compiler& compiled,
		const ShaderSpecialization& specialization, const std::string& entryPoint,
		u32 spvExecutionModel, span<const SpecializationConstantDefault> constantDefaults) {
	compiled.set_entry_point(entryPoint, spv::ExecutionModel(spvExecutionModel));

	auto specConstants = compiled.get_specialization_constants();
	for(auto& sc : specConstants) {
		auto& dst = compiled.get_constant(sc.id);
		dlg_assert(dst.m.columns == 1u);
		dlg_assert(dst.m.c[0].vecsize == 1u);

		auto found = false;
		for(auto& specEntry : specialization.entries) {
			if(specEntry.constantID != sc.constant_id) {
				continue;
			}

			auto data = specialization.data.data() + specEntry.offset;
			dlg_assert(specEntry.size == 4u);
			dlg_assert(specEntry.offset + specEntry.size <= specialization.data.size());
			std::memcpy(&dst.m.c[0].r[0], data, specEntry.size);

			found = true;
			break;
		}

		// if specialization wasn't found, reset it to the default value.
		// needed in case a previous access specialized it.
		if(!found) {
			for(auto& constant : constantDefaults) {
				if(constant.constantID != sc.constant_id) {
					continue;
				}

				dst.m.c[0].r[0] = constant.constant->m.c[0].r[0];
				found = true;
				break;
			}

			dlg_assert(found);
		}
	}
}

spc::Compiler& specializeSpirv(ShaderModule& mod,
		const ShaderSpecialization& specialization, const std::string& entryPoint,
		u32 spvExecutionModel) {
	dlg_assert(mod.compiled);
	auto& compiled = *mod.compiled;
	specializeSpirv(compiled, specialization, entryPoint, spvExecutionModel,
		mod.constantDefaults);
	return compiled;
}

std::unique_ptr<spc::Compiler> copySpecializeSpirv(ShaderModule& mod,
		const ShaderSpecialization& specialization, const std::string& entryPoint,
		u32 spvExecutionModel) {
	dlg_assert(mod.compiled);
	auto compiled = std::make_unique<spc::Compiler>(mod.compiled->get_ir());
	specializeSpirv(*compiled, specialization, entryPoint, spvExecutionModel,
		mod.constantDefaults);
	return compiled;
}

} // namespace vil
