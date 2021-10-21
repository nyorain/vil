#include <gui/shader.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/commandHook.hpp>
#include <gui/cb.hpp>
#include <util/buffmt.hpp>
#include <command/commands.hpp>
#include <threadContext.hpp>
#include <image.hpp>
#include <shader.hpp>
#include <ds.hpp>
#include <numeric>
#include <spvm/types.h>
#include <spvm/ext/GLSL450.h>

namespace vil {

span<spvm_member> children(spvm_member& member) {
	return {member.members, std::size_t(member.member_count)};
}

ShaderDebugger::~ShaderDebugger() {
	unselect();
	if(context) {
		spvm_context_deinitialize(context);
	}
}

void ShaderDebugger::init(Gui& gui) {
	this->gui = &gui;
	context = spvm_context_initialize();

	// TODO: why does this break for games using tcmalloc? tested
	// on linux with dota
	const auto& lang = igt::TextEditor::LanguageDefinition::GLSL();
	textedit.SetLanguageDefinition(lang);

	textedit.SetShowWhitespaces(false);
	textedit.SetTabSize(4);

	// Shader can't be edited
	// textedit.SetHandleKeyboardInputs(false);
	// textedit.SetHandleMouseInputs(false);
}

void ShaderDebugger::select(const spc::Compiler& compiled) {
	unselect();

	this->compiled = &compiled;
	static_assert(sizeof(spvm_word) == sizeof(u32));
	auto ptr = reinterpret_cast<const spvm_word*>(compiled.get_ir().spirv.data());
	program = spvm_program_create(context, ptr, compiled.get_ir().spirv.size());

	initState();

	const char* src {};
	if(program->file_count) {
		src = program->files[0].source;
	}

	if(src) {
		textedit.SetText(src);
	} else {
		textedit.SetText("<No debug code embedded in spirv>");
	}
}

void ShaderDebugger::initState() {
	dlg_assert(program);
	dlg_assert(compiled);

	samplers_.clear();
	images_.clear();

	spvm_state_settings settings {};
	settings.load_variable = [](struct spvm_state* state, unsigned varID,
			unsigned index_count, const spvm_word* indices, spvm_member_list list,
			spvm_word typeID) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		self->loadVar(varID,
			{indices, std::size_t(index_count)},
			{list.members, std::size_t(list.member_count)}, typeID);
	};
	settings.store_variable = [](struct spvm_state* state, unsigned varID,
			unsigned index_count, const spvm_word* indices, spvm_member_list list,
			spvm_word typeID) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		self->storeVar(varID,
			{indices, std::size_t(index_count)},
			{list.members, std::size_t(list.member_count)}, typeID);
	};
	settings.log = [](struct spvm_state*, const char* fmt, va_list vargs) {
		char buf[500];
		std::vsnprintf(buf, sizeof(buf), fmt, vargs);
		dlg_error("spvm: {}", buf);
	};

	state = spvm_state_create(program, settings);
	state->user_data = this;
	state->read_image = [](spvm_state* state, spvm_image* img,
			int x, int y, int z, int layer, int level) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		return self->readImage(*img, x, y, z, layer, level);
	};
	state->write_image = [](spvm_state* state, spvm_image* img,
			int x, int y, int z, int layer, int level, const spvm_vec4f* data) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		self->writeImage(*img, x, y, z, layer, level, *data);
	};

	spvm_word entryPoint = -1;
	for(auto i = 0; i < program->entry_point_count; ++i) {
		if(u32(program->entry_points[i].id) == compiled->get_ir().default_entry_point) {
			entryPoint = program->entry_points[i].id;
			break;
		}
	}

	// TODO: don't static here
	static spvm_ext_opcode_func* glslExt = spvm_build_glsl450_ext();
	spvm_state_set_extension(state, "GLSL.std.450", glslExt);

	dlg_assert(entryPoint != -1);
	spvm_state_prepare(state, entryPoint);
}

void ShaderDebugger::unselect() {
	if(state) {
		spvm_state_delete(state);
		state = nullptr;
	}

	if(program) {
		spvm_program_delete(program);
		program = nullptr;
	}

	compiled = nullptr;
	vars_.clear();
}

void ShaderDebugger::draw() {
	if(!program) {
		imGuiText("No shader selected for debugging");
		return;
	}

	dlg_assert(state);

	// shader view
	if(ImGui::BeginChild("ShaderDebugger", ImVec2(0, -300))) {
		ImGui::PushFont(gui->monoFont);
		textedit.Render("Shader");
		ImGui::PopFont();
	}

	ImGui::EndChild();

	// variable view
	if(ImGui::BeginChild("Vars")) {
		if(state->code_current && ImGui::Button("Step Opcode")) {
			do {
				// auto prev = state->code_current;
				spvm_state_step_opcode(state);

				/*
				for(auto i = 0u; i < program->bound; ++i) {
					auto& res = state->results[i];
					if(res.type != spvm_result_type_constant) {
						continue;
					}

					if(!res.name) {
						continue;
					}

					if(res.source_location >= prev && res.source_location <= state->code_current) {
						vars_.insert_or_assign(res.name, &res);
					}
				}
				*/
			} while(state->current_line < 0);
		}

		if(state->code_current && ImGui::Button("Step Line")) {
			auto line = state->current_line;
			while(state->current_line == line) {
				spvm_state_step_opcode(state);
			}
		}

		if(state->code_current && ImGui::Button("Run")) {
			while(state->code_current) {
				spvm_state_step_opcode(state);
			}
		}

		if(ImGui::Button("Reset")) {
			spvm_state_delete(state);
			initState();
		}

		imGuiText("Current line: {}", state->current_line);
		imGuiText("Instruction count : {}", state->instruction_count);
		imGuiText("Offset: {}", state->code_current - program->code);

		auto printRes = [&](const auto& name, auto* res) {
			imGuiText("{}: ", name);

			ImGui::SameLine();

			// TODO: proper spvm_result formatting
			auto* resType = spvm_state_get_type_info(state->results,
				&state->results[res->pointer]);
			if(resType->value_type == spvm_value_type_vector) {
				auto str = std::string{};

				for(auto i = 0; i < res->member_count; ++i) {
					// TODO:
					auto vt = valueType(res->members[i]);
					if(vt == spvm_value_type_int) {
						str += dlg::format("{}, ", res->members[i].value.s);
					} else if(vt == spvm_value_type_float) {
						str += dlg::format("{}, ", res->members[i].value.f);
					} else {
						str += "??, ";
					}
				}

				imGuiText("{}", str);
			} else if(resType->value_type == spvm_value_type_int) {
				// TODO: bitcount & signedness
				imGuiText("{}", res->members[0].value.s);
			} else if(resType->value_type == spvm_value_type_float) {
				// TODO: bitcount
				imGuiText("{}", res->members[0].value.f);
			} else {
				imGuiText("??");
			}
		};

		// vars_
		for(auto [name, res] : vars_) {
			printRes(name, res);
		}

		// local vars
		for(auto i = 0u; i < program->bound; ++i) {
			auto& res = state->results[i];
			if(res.type != spvm_result_type_variable ||
					res.owner != state->current_function ||
					!res.name) {
				continue;
			}

			auto it = vars_.find(res.name);
			if(it != vars_.end()) {
				continue;
			}

			printRes(std::string_view(res.name), &res);
		}
	}

	ImGui::EndChild();
}

void ShaderDebugger::loadBuiltin(const spc::BuiltInResource& builtin,
		span<const spvm_word> indices, span<spvm_member> dst) {
	dlg_trace("spirv OpLoad of builtin {}", builtin.builtin);

	auto loadVecU = [&](auto& vec) {
		if(indices.empty()) {
			dlg_assert(dst.size() == vec.size());
			for(auto i = 0u; i < vec.size(); ++i) {
				dlg_assert(valueType(dst[i]) == spvm_value_type_int);
				dst[i].value.u = vec[i];
			}
		} else {
			dlg_assert(indices.size() == 1u);
			dlg_assert(dst.size() == 1u);
			dlg_assert(u32(indices[0]) < vec.size());
			dlg_assert(valueType(dst[0]) == spvm_value_type_int);
			dst[0].value.u = vec[indices[0]];
		}
	};

	auto loadScalarU = [&](auto& val) {
		dlg_assert(dst.size() == 1u);
		dlg_assert(valueType(dst[0]) == spvm_value_type_int);
		dst[0].value.u = val;
	};

	switch(builtin.builtin) {
		case spv::BuiltInWorkgroupSize: {
			Vec3ui size {8u, 8u, 1u}; // TODO
			loadVecU(size);
			break;
		}
		case spv::BuiltInWorkgroupId:
		case spv::BuiltInInvocationId:
		case spv::BuiltInGlobalInvocationId:
		case spv::BuiltInLocalInvocationId: {
			Vec3ui id {0u, 0u, 0u}; // TODO
			loadVecU(id);
			break;
		}
		case spv::BuiltInLocalInvocationIndex: {
			auto id = 0u; // TODO
			loadScalarU(id);
			break;
		} default:
			dlg_error("Unhandled builtin: {}", builtin.builtin);
			break;
	}
}

void ShaderDebugger::loadVar(unsigned srcID, span<const spvm_word> indices,
		span<spvm_member> dst, u32 typeID) {
	dlg_assert(compiled);

	auto res = resource(*this->compiled, srcID);
	if(!res) {
		auto builtin = builtinResource(*this->compiled, srcID);
		if(!builtin) {
			dlg_error("OpLoad of invalid/unknown var {}", srcID);
			return;
		}

		loadBuiltin(*builtin, indices, dst);
		return;
	}

	dlg_trace("spirv OpLoad of var {}", srcID);
	dlg_assert(compiled->has_decoration(res->id, spv::DecorationDescriptorSet) &&
				compiled->has_decoration(res->id, spv::DecorationBinding));
	auto& spcTypeMaybeArrayed = compiled->get_type(res->type_id);

	// Handle array bindings
	auto arrayElemID = 0u;
	auto spcType = spcTypeMaybeArrayed;
	if(!spcTypeMaybeArrayed.array.empty()) {
		// Multidimensional binding arrays not allowed I guess
		dlg_assert(spcType.array.size() == 1u);

		// Loading an entire binding array is not allowed I guess, we just
		// require an element to be selected here
		dlg_assert(!indices.empty());

		auto bounds = spcType.array[0];
		if(spcType.array_size_literal[0] == true) {
			bounds = compiled->evaluate_constant_u32(bounds);
		}

		dlg_assert(u32(indices[0]) < bounds);

		arrayElemID = indices[0];
		indices = indices.subspan(1u);
		spcType.array.clear();
		spcType.array_size_literal.clear();
	}

	auto setID = compiled->get_decoration(res->id, spv::DecorationDescriptorSet);
	auto bindingID = compiled->get_decoration(res->id, spv::DecorationBinding);

	auto* baseCmd = gui->cbGui().commandViewer().command();
	auto* cmd = static_cast<const StateCmdBase*>(baseCmd);

	auto& dsState = gui->cbGui().commandViewer().dsState();
	auto dss = cmd->boundDescriptors().descriptorSets;

	dlg_assert(setID < dss.size());
	auto& cmdDS = dss[setID];

	auto stateIt = dsState.states.find(cmdDS.ds);
	dlg_assert(stateIt != dsState.states.end());
	auto& ds = *stateIt->second;

	// For samplers, we didn't do a copy and so have to early-out here
	auto dsCopyIt = varIDToDsCopyMap.find(srcID);
	if(dsCopyIt == varIDToDsCopyMap.end()) {
		dlg_assert_or(spcType.basetype == spc::SPIRType::Sampler, return);
		dlg_assert_or(spcType.storage == spv::StorageClassUniform, return);
		dlg_assert(indices.empty());

		dlg_trace(" >> found sampler");

		auto image = vil::images(ds, bindingID)[arrayElemID];
		dlg_assert(image.sampler);

		auto& sampler = samplers_.emplace_back();
		sampler.desc = setupSampler(*image.sampler);

		dlg_assert(dst.size() == 1u);
		dst[0].value.sampler = &sampler;

		dlg_assert(dst[0].member_count == 0u);
		return;
	}

	auto hookState = gui->cbGui().commandViewer().state();
	dlg_assert(hookState);
	dlg_assert(hookState->copiedDescriptors.size() > dsCopyIt->second);
	dlg_assert(gui->dev().commandHook->descriptorCopies.size() > dsCopyIt->second);
	auto& copyRequest = gui->dev().commandHook->descriptorCopies[dsCopyIt->second];
	// dsCopyIt->second stores the beginning of the range we stored in the
	// array elements in. So we have to offset it with the arrayElemID
	auto& copyResult = hookState->copiedDescriptors[dsCopyIt->second + arrayElemID];

	dlg_assert(copyRequest.set == setID);
	dlg_assert(copyRequest.binding == bindingID);

	if(spcType.storage == spv::StorageClassPushConstant) {
		// TODO: get from command
		dlg_error("TODO: loadVar push constant");
		return;
	} else if(spcType.storage == spv::StorageClassUniformConstant) {
		// we already handled samplers above
		dlg_assert(spcType.basetype != spc::SPIRType::Sampler);
		if(spcType.basetype == spc::SPIRType::Image) {
			dlg_trace(" >> image");
			auto image = vil::images(ds, bindingID)[arrayElemID];
			auto& imgView = nonNull(image.imageView);
			auto& img = nonNull(imgView.img);

			auto* buf = std::get_if<OwnBuffer>(&copyResult.data);
			dlg_assert(buf);

			auto& dstImg = images_.emplace_back();
			dstImg.width = img.ci.extent.width;
			dstImg.height = img.ci.extent.height;
			dstImg.depth = img.ci.extent.depth;
			dstImg.levels = imgView.ci.subresourceRange.levelCount;
			dstImg.layers = imgView.ci.subresourceRange.layerCount;
			dstImg.data = buf->data();

			dlg_assert(dst.size() == 1u);

			dst[0].value.image = &dstImg;
			dlg_assert(u32(dst[0].type) == res->base_type_id);
			dlg_assert(dst[0].member_count == 0u);

			return;
		} else if(spcType.basetype == spc::SPIRType::SampledImage) {
			dlg_trace(" >> sampled image");

			auto image = vil::images(ds, bindingID)[arrayElemID];
			auto& imgView = nonNull(image.imageView);
			auto& img = nonNull(imgView.img);

			auto* buf = std::get_if<OwnBuffer>(&copyResult.data);
			dlg_assert(buf);

			auto& dstImg = images_.emplace_back();
			dstImg.width = img.ci.extent.width;
			dstImg.height = img.ci.extent.height;
			dstImg.depth = img.ci.extent.depth;
			dstImg.levels = imgView.ci.subresourceRange.levelCount;
			dstImg.layers = imgView.ci.subresourceRange.layerCount;
			dstImg.data = buf->data();

			auto& sampler = samplers_.emplace_back();
			sampler.desc = setupSampler(*image.sampler);

			auto& spvmRes = state->results[srcID];
			auto* resType = spvm_state_get_type_info(state->results, &state->results[spvmRes.pointer]);

			dlg_assert(dst.size() == 2u);

			// members[0]: image
			dlg_assert(dst[0].member_count == 0u);
			dlg_assert(dst[0].type == resType->pointer);
			dst[0].value.image = &dstImg;

			// members[0]: sampler. We don't care about the OpTypeSampler used
			dlg_assert(dst[1].member_count == 0u);
			dst[1].value.sampler = &sampler;

			return;
		} else {
			dlg_error("Invalid/unsupported UniformConstant");
			return;
		}
	} else if(spcType.storage == spv::StorageClassUniform ||
			spcType.storage == spv::StorageClassStorageBuffer) {
		if(spcType.basetype == spc::SPIRType::Struct) {
			dlg_trace(" >> struct");

			ThreadMemScope tms;
			const auto* type = buildType(*compiled, res->type_id, tms);
			dlg_assert(type);

			auto* buf = std::get_if<OwnBuffer>(&copyResult.data);
			dlg_assert(buf);
			auto data = buf->data();

			auto off = 0u;
			while(!indices.empty()) {
				if(!type->array.empty()) {
					auto count = type->array[0];
					auto remDims = span<const u32>(type->array).subspan(1);

					auto subSize = type->deco.arrayStride;
					dlg_assert(subSize);

					for(auto size : remDims) {
						dlg_assert(size != 0u); // only first dimension can be runtime size
						subSize *= size;
					}

					if(count == 0u) { // runtime array, find out size
						auto remSize = data.size() - off;
						// doesn't have to be like that even though it's sus if the buffer
						// size isn't a multiple of the stride.
						// dlg_assert(remSize % subSize == 0u);
						count = remSize / subSize; // intentionally round down
					}

					off += u32(indices[0]) * subSize;
					indices = indices.subspan(1);

					while(!indices.empty() && !remDims.empty()) {
						count = remDims[0];
						remDims = remDims.subspan(1);

						subSize /= count;
						off += u32(indices[0]) * subSize;
						indices = indices.subspan(1);
					}

					auto cpy = *tms.allocRaw<Type>();
					cpy = *type;
					cpy.array = {remDims.begin(), remDims.end()};
					type = &cpy;
				} else if(type->type == Type::typeStruct) {
					auto id = u32(indices[0]);
					indices = indices.subspan(1);
					dlg_assert(id < type->members.size());

					auto& member = type->members[id];
					off += member.offset;
					type = member.type;
				} else if(type->columns > 1) {
					auto id = u32(indices[0]);
					indices = indices.subspan(1);
					off += type->deco.matrixStride * id;
					dlg_assert(id < type->columns);

					auto cpy = *tms.allocRaw<Type>();
					cpy = *type;
					cpy.columns = 1u;
					type = &cpy;
				} else if(type->vecsize > 1) {
					auto id = u32(indices[0]);
					indices = indices.subspan(1);
					dlg_assert(id < type->vecsize);

					auto cpy = *tms.allocRaw<Type>();
					cpy = *type;
					cpy.vecsize = 1u;
					type = &cpy;

					// buffer layout does not matter here since new type is scalar
					off += id * size(*type, BufferLayout::std140);
				} else {
					dlg_error("Invalida type for AccessChain");
					type = nullptr;
					break;
				}
			}

			dlg_assert(type);

			spvm_member* setupDst;
			spvm_member wrapper;
			if(type->type == Type::typeStruct ||
					type->vecsize > 1 ||
					type->columns > 1 ||
					!type->array.empty()) {
				setupDst = &wrapper;
				wrapper.type = typeID;
				wrapper.members = dst.data();
				wrapper.member_count = dst.size();
			} else {
				dlg_assert(dst.size() == 1u);
				setupDst = &dst[0];
			}

			setupMember(*type, data.subspan(off), *setupDst);
			return;
		} else {
			dlg_error("Unsupported spc type for uniform value");
			return;
		}
	}

	// TODO:
	// - graphic pipeline input/output vars
	// - ray tracing stuff
	dlg_error("Unsupported variable storage class");
	return;
}

void ShaderDebugger::storeVar(unsigned id, span<const spvm_word> indices,
			span<spvm_member> src, u32 typeID) {
	// TODO
	// even if we are not interested in the results, we have to implement
	// it to make sure reads of previous writes return the written values.
	(void) id;
	(void) indices;
	(void) src;
	(void) typeID;
}

void ShaderDebugger::updateHooks(CommandHook& hook) {
	dlg_assert(compiled);

	hook.unsetHookOps();
	varIDToDsCopyMap.clear();

	auto resources = compiled->get_shader_resources();

	auto addCopies = [&](auto& resources, bool bufferAsImage) {
		for(auto& res : resources) {
			if(!compiled->has_decoration(res.id, spv::DecorationDescriptorSet) ||
					!compiled->has_decoration(res.id, spv::DecorationBinding)) {
				dlg_warn("resource {} doesn't have set/binding decorations", res.name);
				continue;
			}

			auto& type = compiled->get_type_from_variable(res.id);
			varIDToDsCopyMap.insert({u32(res.id), u32(hook.descriptorCopies.size())});

			CommandHook::DescriptorCopy dsCopy;
			dsCopy.set = compiled->get_decoration(res.id, spv::DecorationDescriptorSet);
			dsCopy.binding = compiled->get_decoration(res.id, spv::DecorationBinding);
			dsCopy.before = true;
			dsCopy.imageAsBuffer = bufferAsImage;

			auto arraySize = 1u;
			if(type.array.size() > 1) {
				// TODO: support all array dimensions
				dlg_assert(type.array.size() == 1u);
			}

			for(auto i = 0u; i < arraySize; ++i) {
				dsCopy.elem = i;
				hook.descriptorCopies.push_back(dsCopy);
			}
		}
	};

	addCopies(resources.sampled_images, true);
	addCopies(resources.separate_images, true);
	addCopies(resources.storage_images, true);
	// addCopies(resources.separate_samplers);
	addCopies(resources.storage_buffers, false);
	addCopies(resources.uniform_buffers, false);
	addCopies(resources.subpass_inputs, true);
}

void ShaderDebugger::setupScalar(const Type& type, ReadBuf data, spvm_member& dst) {
	[[maybe_unused]] auto vt = valueType(dst);
	dlg_assert(dst.member_count == 0u);
	dlg_assert(vt == spvm_value_type_bool ||
		vt == spvm_value_type_float ||
		vt == spvm_value_type_int);

	if(type.type == Type::typeFloat) {
		dlg_assert(vt == spvm_value_type_float);

		if(type.width == 32) {
			dst.value.f = read<float>(data);
		} else if(type.width == 64) {
			dst.value.d = read<double>(data);
		} else {
			dlg_error("Invalid width");
		}
	} else if(type.type == Type::typeInt) {
		dlg_assert(vt == spvm_value_type_int);

		if(type.width == 32) {
			dst.value.s = read<i32>(data);
		} else {
			dlg_error("Invalid width");
		}
	} else if(type.type == Type::typeUint) {
		dlg_assert(vt == spvm_value_type_int);

		if(type.width == 32) {
			dst.value.s = read<u32>(data);
		} else {
			dlg_error("Invalid width");
		}
	} else if(type.type == Type::typeBool) {
		dlg_assert(vt == spvm_value_type_bool);
		dlg_assert(type.width == 32);
		dst.value.b = bool(read<u32>(data));
	} else {
		dlg_error("Invalid scalar");
	}
}

void ShaderDebugger::setupVector(const Type& type, u32 stride, ReadBuf data,
		spvm_member& dst) {
	dlg_assert(valueType(dst) == spvm_value_type_vector);

	auto nextType = type;
	nextType.vecsize = 1u;

	auto elems = children(dst);
	dlg_assert(elems.size() == type.vecsize);

	for(auto i = 0u; i < type.vecsize; ++i) {
		setupScalar(type, data.subspan(i * stride), elems[i]);
	}
}

void ShaderDebugger::setupMember(const Type& type, ReadBuf data, spvm_member& dst) {
	if(!type.array.empty()) {
		setupMemberArray(type.array, type, data, dst);
		return;
	}

	if(type.type == Type::typeStruct) {
		dlg_assert(valueType(dst) == spvm_value_type_struct);

		auto members = children(dst);
		dlg_assert(members.size() == type.members.size());

		for(auto [i, member] : enumerate(type.members)) {
			setupMember(*member.type, data.subspan(member.offset), members[i]);
		}

		return;
	}

	if(type.columns > 1) {
		dlg_assert(type.deco.matrixStride);
		dlg_assert(valueType(dst) == spvm_value_type_matrix);

		auto nextType = type;
		nextType.columns = 1u;

		auto colStride = type.deco.matrixStride;
		auto rowStride = type.width / 8;

		if(type.deco.flags & Decoration::Bits::rowMajor) {
			std::swap(colStride, rowStride);
		}

		auto cols = children(dst);
		dlg_assert(cols.size() == type.columns);

		for(auto i = 0u; i < type.columns; ++i) {
			auto off = i * colStride;
			setupVector(nextType, rowStride, data.subspan(off), cols[i]);
		}

		return;
	} else if(type.vecsize > 1) {
		auto stride = type.width / 8;
		return setupVector(type, stride, data, dst);
	}

	setupScalar(type, data, dst);
}

void ShaderDebugger::setupMemberArray(span<const u32> arrayDims,
		const Type& type, ReadBuf data, spvm_member& dst) {
	dlg_assert(!type.array.empty());

	auto count = arrayDims[0];
	arrayDims = arrayDims.subspan(1);

	auto subSize = type.deco.arrayStride;
	for(auto dim : arrayDims) {
		dlg_assert(dim != 0u); // only first dimension can be runtime size
		subSize *= dim;
	}

	if(count == 0u) {
		// NOTE: this can't happen, loading a runtime_array is not allowed
		// per spirv.
		dlg_assert(valueType(dst) == spvm_value_type_runtime_array);
		dlg_error("OpLoad on runtime array?! Not allowed!");
		return;

		/*
		// runtime array, find out real size.
		auto remSize = data.size();
		// doesn't have to be like that even though it's sus if the buffer
		// size isn't a multiple of the stride.
		// dlg_assert(remSize % subSize == 0u);
		count = remSize / subSize; // intentionally round down
		*/
	}

	dlg_assert(valueType(dst) == spvm_value_type_array);

	auto elems = children(dst);
	dlg_assert(elems.size() == count);

	for(auto i = 0u; i < count; ++i) {
		auto nextData = data.subspan(i * subSize);

		if(!arrayDims.empty()) {
			setupMemberArray(arrayDims, type, nextData, elems[i]);
		} else {
			auto nt = type;
			nt.array.clear();
			setupMember(nt, nextData, elems[i]);
		}
	}
}

spvm_sampler_desc ShaderDebugger::setupSampler(const Sampler& src) {
	spvm_sampler_desc ret {};
	ret.filter_min = spvm_sampler_filter(src.ci.minFilter);
	ret.filter_mag = spvm_sampler_filter(src.ci.magFilter);
	ret.mipmap_mode = spvm_sampler_filter(src.ci.mipmapMode);
	ret.address_mode_u = spvm_sampler_address_mode(src.ci.addressModeU);
	ret.address_mode_v = spvm_sampler_address_mode(src.ci.addressModeV);
	ret.address_mode_w = spvm_sampler_address_mode(src.ci.addressModeW);
	ret.mip_bias = src.ci.mipLodBias;
	ret.compare_op = spvm_sampler_compare_op(src.ci.compareOp);
	ret.min_lod = src.ci.minLod;
	ret.max_lod = src.ci.maxLod;
	return ret;
}

spvm_vec4f ShaderDebugger::readImage(spvm_image& srcImg, int x, int y, int z, int layer, int level) {
	auto& img = static_cast<OurImage&>(srcImg);
	auto off = 0u;

	Vec3ui extent = {img.width, img.height, img.depth};
	for(auto l = 0; l < level; ++l) {
		extent[0] = std::max(extent[0] >> 1, 1u);
		extent[1] = std::max(extent[1] >> 1, 1u);
		extent[2] = std::max(extent[2] >> 1, 1u);
		off += extent[0] * extent[1] * extent[2] * img.layers;
	}

	auto sliceSize = extent[0] * extent[1];
	auto layerSize = extent[2] * sliceSize;

	off += layer * layerSize;
	off += z * sliceSize;
	off += y * extent[0];
	off += x;

	auto texel = img.data.subspan(off * sizeof(spvm_vec4f));
	return read<spvm_vec4f>(texel);
}

void ShaderDebugger::writeImage(spvm_image&, int x, int y, int z, int layer, int level,
		const spvm_vec4f& data) {
	// TODO
	// have to implement it, make sure subsequent reads return the same value
	(void) x;
	(void) y;
	(void) z;
	(void) layer;
	(void) level;
	(void) data;
}

spvm_value_type ShaderDebugger::valueType(spvm_member& member) {
	auto* resType = spvm_state_get_type_info(state->results, &state->results[member.type]);
	dlg_assert(resType);
	return resType->value_type;
}

} // namespace vil