#include <gui/shader.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/commandHook.hpp>
#include <util/buffmt.hpp>
#include <command/commands.hpp>
#include <threadContext.hpp>
#include <image.hpp>
#include <shader.hpp>
#include <ds.hpp>
#include <numeric>

namespace vil {

ShaderDebugger::~ShaderDebugger() {
	if(state) {
		spvm_state_delete(state);
	}

	if(program) {
		spvm_program_delete(program);
	}
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
}

void ShaderDebugger::select(const spc::Compiler& compiled) {
	if(state) {
		spvm_state_delete(state);
		state = nullptr;
	}

	if(program) {
		spvm_program_delete(program);
		program = nullptr;
	}

	this->compiled = &compiled;
	static_assert(sizeof(spvm_word) == sizeof(u32));
	auto ptr = reinterpret_cast<const spvm_word*>(compiled.get_ir().spirv.data());
	program = spvm_program_create(context, ptr, compiled.get_ir().spirv.size());

	spvm_state_settings settings {};
	settings.load_variable = [](struct spvm_state* state, unsigned varID,
			unsigned index_count, const spvm_word* indices) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		return self->loadVar(varID, {indices, index_count});
	};
	settings.store_variable = [](struct spvm_state* state, unsigned varID,
			unsigned index_count, const spvm_word* indices, spvm_member_list list) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		self->storeVar(varID, {indices, index_count}, list);
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

void ShaderDebugger::draw() {
	if(!program) {
		imGuiText("No shader selected for debugging");
		return;
	}

	ImGui::PushFont(gui->monoFont);
	textedit.Render("Shader");
	ImGui::PopFont();
}

spvm_member_list ShaderDebugger::loadVar(unsigned id, span<const spvm_word> indices) {
	dlg_assert(compiled);

	auto res = resource(*this->compiled, id);
	dlg_assert(res);

	auto it = varIDToDsCopyMap.find(id);
	dlg_assert(it != varIDToDsCopyMap.end());

	auto hookState = gui->cbGui().commandViewer().state();
	dlg_assert(hookState);
	dlg_assert(hookState->copiedDescriptors.size() > it->second);
	dlg_assert(gui->dev().commandHook->descriptorCopies.size() > it->second);
	auto& copyRequest = gui->dev().commandHook->descriptorCopies[it->second];
	auto& copyResult = hookState->copiedDescriptors[it->second];

	auto setID = copyRequest.set;
	auto bindingID = copyRequest.binding;
	auto elemID = copyRequest.elem;

	auto* baseCmd = gui->cbGui().commandViewer().command();
	auto* cmd = static_cast<const StateCmdBase*>(baseCmd);

	auto& dsState = gui->cbGui().commandViewer().dsState();
	auto dss = cmd->boundDescriptors().descriptorSets;

	dlg_assert(setID < dss.size());
	auto& cmdDS = dss[setID];

	auto stateIt = dsState.states.find(cmdDS.ds);
	dlg_assert(stateIt != dsState.states.end());
	auto& ds = *stateIt->second;

	auto& spcType = compiled->get_type(res->type_id);
	if(spcType.storage == spv::StorageClassPushConstant) {
		// TODO: get from command
		return {};
	} else if(spcType.storage == spv::StorageClassUniform ||
			spcType.storage == spv::StorageClassStorageBuffer) {

		if(spcType.basetype == spc::SPIRType::Sampler) {
			auto image = vil::images(ds, bindingID)[elemID];
			dlg_assert(image.sampler);

			auto& sampler = samplers_.emplace_back();
			sampler.desc = setupSampler(*image.sampler);
			sampler.user_data = &sampler.desc;

			auto& member = members_.emplace_back().emplace_back();
			member.type = res->base_type_id;
			member.member_count = 0u;
			member.value.sampler = &sampler;

			return {1u, &member};
		} else if(spcType.basetype == spc::SPIRType::Image) {
			auto image = vil::images(ds, bindingID)[elemID];
			auto& imgView = nonNull(image.imageView);
			auto& img = nonNull(imgView.img);

			auto* buf = std::get_if<OwnBuffer>(&copyResult.data);
			dlg_assert(buf);

			auto& dst = images_.emplace_back();
			dst.width = img.ci.extent.width;
			dst.height = img.ci.extent.height;
			dst.depth = img.ci.extent.depth;
			dst.levels = imgView.ci.subresourceRange.levelCount;
			dst.layers = imgView.ci.subresourceRange.layerCount;
			dst.data = buf->data();

			auto& member = members_.emplace_back().emplace_back();
			member.type = res->base_type_id;
			member.member_count = 0u;
			member.value.image = &dst;

			return {1u, &member};
		} else if(spcType.basetype == spc::SPIRType::SampledImage) {
			auto image = vil::images(ds, bindingID)[elemID];
			auto& imgView = nonNull(image.imageView);
			auto& img = nonNull(imgView.img);

			auto* buf = std::get_if<OwnBuffer>(&copyResult.data);
			dlg_assert(buf);

			auto& dst = images_.emplace_back();
			dst.width = img.ci.extent.width;
			dst.height = img.ci.extent.height;
			dst.depth = img.ci.extent.depth;
			dst.levels = imgView.ci.subresourceRange.levelCount;
			dst.layers = imgView.ci.subresourceRange.layerCount;
			dst.data = buf->data();

			auto& sampler = samplers_.emplace_back();
			sampler.desc = setupSampler(*image.sampler);
			sampler.user_data = &sampler.desc;

			auto& members = members_.emplace_back();
			members.resize(2);

			auto& spvmRes = state->results[id];
			auto* resType = spvm_state_get_type_info(state->results, &state->results[spvmRes.pointer]);

			members[0].type = resType->pointer; // pointer to image type id
			members[0].member_count = 0u;
			members[0].value.image = &dst;

			members[1].type = spvm_word(-1); // we don't care about OpTypeSampler
			members[1].member_count = 0u;
			members[1].value.sampler = &sampler;

			return {u32(members.size()), members.data()};
		} else if(spcType.basetype == spc::SPIRType::Struct) {
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

					for(auto size : type->array) {
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
					dlg_assert("Invalida type for AccessChain");
					type = nullptr;
					break;
				}
			}

			dlg_assert(type);
			auto res = setupMembers(*type, data.subspan(off));

			if(res.member_count != 0) {
				return {unsigned(res.member_count), res.members};
			} else {
				auto& persistent = members_.emplace_back().emplace_back(res);
				return {1u, &persistent};
			}
		} else {
			dlg_assert("Unsupported spc type for uniform value");
			return {};
		}
	}

	// TODO:
	// - graphic pipeline input/output vars
	// - ray tracing stuff
	dlg_assert("Unsupported variable storage class");
	return {};
}

void ShaderDebugger::storeVar(unsigned id, span<const spvm_word> indices,
			spvm_member_list) {
	// TODO
	// even if we are not interested in the results, we have to implement
	// it to make sure reads of previous writes return the written values.
	(void) id;
	(void) indices;
}

void ShaderDebugger::updateHooks(CommandHook& hook) {
	dlg_assert(compiled);

	hook.unsetHookOps();
	varIDToDsCopyMap.clear();

	auto resources = compiled->get_shader_resources();

	auto addCopies = [&](auto& resources) {
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
			dsCopy.imageAsBuffer = true;
			dsCopy.before = true;

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

	addCopies(resources.sampled_images);
	addCopies(resources.separate_images);
	addCopies(resources.separate_samplers);
	addCopies(resources.storage_buffers);
	addCopies(resources.uniform_buffers);
	addCopies(resources.subpass_inputs);
}

spvm_member ShaderDebugger::setupScalar(const Type& type, ReadBuf data) {
	auto dst = spvm_member {};

	if(type.type == Type::typeFloat) {
		dst.type = spvm_value_type_float;
		if(type.width == 32) {
			dst.value.f = read<float>(data);
		} else if(type.width == 64) {
			dst.value.d = read<double>(data);
		} else {
			dlg_error("Invalid width");
		}
	} else if(type.type == Type::typeInt) {
		dst.type = spvm_value_type_int;
		if(type.width == 32) {
			dst.value.s = read<i32>(data);
		} else {
			dlg_error("Invalid width");
		}
	} else if(type.type == Type::typeUint) {
		dst.type = spvm_value_type_int;
		if(type.width == 32) {
			dst.value.s = read<u32>(data);
		} else {
			dlg_error("Invalid width");
		}
	} else if(type.type == Type::typeBool) {
		dst.type = spvm_value_type_bool;
		dlg_assert(type.width == 32);
		dst.value.b = bool(read<u32>(data));
	} else {
		dlg_error("Invalid scalar");
	}

	return dst;
}

spvm_member ShaderDebugger::setupVector(const Type& type, u32 stride, ReadBuf data) {
	auto dst = spvm_member {};
	dst.type = spvm_value_type_vector;

	auto nextType = type;
	nextType.vecsize = 1u;

	auto& members = members_.emplace_back();
	members.reserve(type.vecsize);
	for(auto i = 0u; i < type.vecsize; ++i) {
		setupScalar(type, data.subspan(i * stride));
	}

	dst.member_count = members.size();
	dst.members = members.data();
	return dst;
}

spvm_member ShaderDebugger::setupMembers(const Type& type, ReadBuf data) {
	if(!type.array.empty()) {
		return setupMembersArray(type.array, type, data);
	}

	if(type.type == Type::typeStruct) {
		auto dst = spvm_member {};
		dst.type = spvm_value_type_struct;

		auto& members = members_.emplace_back();
		members.reserve(type.members.size());
		for(auto& member : type.members) {
			auto mem = setupMembers(*member.type, data.subspan(member.offset));
			members.push_back(mem);
		}

		dst.member_count = members.size();
		dst.members = members.data();
		return dst;
	}

	if(type.columns) {
		dlg_assert(type.deco.matrixStride);
		auto dst = spvm_member {};
		dst.type = spvm_value_type_matrix;

		auto nextType = type;
		nextType.columns = 1u;

		auto colStride = type.deco.matrixStride;
		auto rowStride = type.width / 8;

		if(type.deco.flags & Decoration::Bits::rowMajor) {
			std::swap(colStride, rowStride);
		}

		auto& members = members_.emplace_back();
		members.reserve(type.columns);
		for(auto i = 0u; i < type.columns; ++i) {
			auto off = i * colStride;
			auto mem = setupVector(nextType, rowStride, data.subspan(off));
			members.push_back(mem);
		}

		dst.member_count = members.size();
		dst.members = members.data();
		return dst;
	} else if(type.vecsize) {
		auto stride = type.width / 8;
		return setupVector(type, stride, data);
	}

	return setupScalar(type, data);
}

spvm_member ShaderDebugger::setupMembersArray(span<const u32> arrayDims,
		const Type& type, ReadBuf data) {
	auto dst = spvm_member {};

	auto count = arrayDims[0];
	arrayDims = arrayDims.subspan(1);

	auto subSize = type.deco.arrayStride;
	for(auto dim : arrayDims) {
		dlg_assert(dim != 0u); // only first dimension can be runtime size
		subSize *= dim;
	}

	if(count == 0u) {
		dst.type = spvm_value_type_runtime_array;

		// runtime array, find out real size.
		auto remSize = data.size();
		// doesn't have to be like that even though it's sus if the buffer
		// size isn't a multiple of the stride.
		// dlg_assert(remSize % subSize == 0u);
		count = remSize / subSize; // intentionally round down
	} else {
		dst.type = spvm_value_type_array;
	}

	auto& members = members_.emplace_back();
	members.reserve(count);
	for(auto i = 0u; i < count; ++i) {
		auto nextData = data.subspan(i * subSize);

		auto& mem = members.emplace_back();
		if(!arrayDims.empty()) {
			mem = setupMembersArray(arrayDims, type, nextData);
		} else {
			auto nt = type;
			nt.array.clear();
			mem = setupMembers(nt, nextData);
		}
	}

	dst.member_count = members.size();
	dst.members = members.data();

	return dst;

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
	ret.compare_enable = src.ci.compareEnable;
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

} // namespace vil
