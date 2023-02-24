#ifndef IMGUI_DEFINE_MATH_OPERATORS
	#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/shader.hpp>
#include <gui/gui.hpp>
#include <gui/util.hpp>
#include <gui/cb.hpp>
#include <commandHook/hook.hpp>
#include <commandHook/record.hpp>
#include <util/buffmt.hpp>
#include <util/profiling.hpp>
#include <util/fmt.hpp>
#include <util/f16.hpp>
#include <command/commands.hpp>
#include <threadContext.hpp>
#include <device.hpp>
#include <image.hpp>
#include <shader.hpp>
#include <ds.hpp>
#include <numeric>
#include <spirv-cross/spirv_cross.hpp>
#include <spvm/types.h>
#include <spvm/ext/GLSL450.h>
#include <vk/format_utils.h>
#include <imgui/imgui_internal.h>
#include <vil_api.h>

namespace vil {

using nytl::read;

const ShaderDebugger::OurImage ShaderDebugger::emptyImage {};
const spvm_sampler ShaderDebugger::defaultSampler {};

span<spvm_member> children(spvm_member& member) {
	return {member.members, std::size_t(member.member_count)};
}

ShaderDebugger::ShaderDebugger() = default;

ShaderDebugger::~ShaderDebugger() {
	unselect();
	if(spvm_.context) {
		spvm_context_deinitialize(spvm_.context);
	}
}

void ShaderDebugger::init(Gui& gui) {
	this->gui_ = &gui;
	spvm_.context = spvm_context_initialize();

	// TODO: decide dynamically whether to use GLSL or HLSL here.
	// Can probalby parse it from the shader module
	const auto& lang = igt::TextEditor::LanguageDefinition::GLSLFast();
	textedit_.SetLanguageDefinition(lang);

	textedit_.SetShowWhitespaces(false);
	textedit_.SetTabSize(4);
	textedit_.SetReadOnly(true);
}

void ShaderDebugger::select(std::unique_ptr<spc::Compiler> compiled) {
	unselect();

	this->compiled_ = std::move(compiled);

	static_assert(sizeof(spvm_word) == sizeof(u32));
	auto ptr = reinterpret_cast<const spvm_word*>(compiled_->get_ir().spirv.data());
	spvm_.program = spvm_program_create(spvm_.context, ptr, compiled_->get_ir().spirv.size());

	initState();

	const char* src {};
	if(spvm_.program->file_count) {
		src = spvm_.program->files[0].source;
	}

	if(src) {
		textedit_.SetText(src);
	} else {
		textedit_.SetText("<No debug code embedded in spirv>");
	}
}

void ShaderDebugger::initState() {
	dlg_assert(spvm_.program);
	dlg_assert(compiled_);

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

	spvm_.state = spvm_state_create(spvm_.program, settings);
	spvm_.state->user_data = this;
	spvm_.state->read_image = [](spvm_state* state, spvm_image* img,
			int x, int y, int z, int layer, int level) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		return self->readImage(*img, x, y, z, layer, level);
	};
	spvm_.state->write_image = [](spvm_state* state, spvm_image* img,
			int x, int y, int z, int layer, int level, const spvm_vec4f* data) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		self->writeImage(*img, x, y, z, layer, level, *data);
	};
	spvm_.state->array_length = [](spvm_state* state, unsigned varID,
			unsigned index_count, const spvm_word* indices) {
		auto* self = static_cast<ShaderDebugger*>(state->user_data);
		return self->arrayLength(varID, {indices, std::size_t(index_count)});
	};

	static spvm_analyzer analyzer;
	analyzer.on_undefined_behavior = [](spvm_state*, spvm_word ub) {
		// TODO: allow to break on UB.
		dlg_trace("shader triggered undefined behavior: {}", ub);
	};
	spvm_.state->analyzer = &analyzer;

	spvm_word entryPoint = -1;
	for(auto i = 0; i < spvm_.program->entry_point_count; ++i) {
		if(u32(spvm_.program->entry_points[i].id) == compiled_->get_ir().default_entry_point) {
			entryPoint = spvm_.program->entry_points[i].id;
			break;
		}
	}

	// TODO: don't static here
	static spvm_ext_opcode_func* glslExt = spvm_build_glsl450_ext();
	spvm_state_set_extension(spvm_.state, "GLSL.std.450", glslExt);

	dlg_assert(entryPoint != -1);
	spvm_state_prepare(spvm_.state, entryPoint);
}

void ShaderDebugger::unselect() {
	currLine_ = {};
	currFileName_ = {};
	varIDToDsCopyMap_.clear();
	samplers_.clear();
	images_.clear();

	if(spvm_.state) {
		spvm_state_delete(spvm_.state);
		spvm_.state = nullptr;
	}

	if(spvm_.program) {
		spvm_program_delete(spvm_.program);
		spvm_.program = nullptr;
	}

	compiled_ = {};
	breakpoints_.clear();
}

void ShaderDebugger::draw() {
	if(!spvm_.program) {
		imGuiText("No shader selected for debugging");
		return;
	}

	dlg_assert(spvm_.state);
	auto hookState = selection().completedHookState();

	if(!hookState) {
		imGuiText("Waiting for submission using the shader...");
		return;
	}

	auto isLastReturn = [this](){
		// TODO: kinda hacky. I guess spvm should have a function
		// like this? or we should always save the relevant information
		// before stepping? not sure.
		spvm_word opcode_data = *spvm_.state->code_current;
		SpvOp opcode = SpvOp(opcode_data & SpvOpCodeMask);

		if((opcode == SpvOpReturn || opcode == SpvOpReturnValue) &&
				spvm_.state->function_stack_current == 0) {
			return true;
		}

		return false;
	};

	if(lastHookState_ != hookState.get()) {
		rerun_ = true;
		lastHookState_ = hookState.get();
	}

	if(rerun_) {
		spvm_state_delete(spvm_.state);
		initState();

		// TODO: not sure how to handle divergence (i.e. when the current
		// run doesn't hit the position we want to get to)
		// Maybe just go to beginning again but don't overwrite currLine,
		// currFileName?
		// notify about it in UI?
		while(spvm_.state->code_current && (
					u32(spvm_.state->current_line) != currLine_ ||
					!spvm_.state->current_file ||
					spvm_.state->current_file != currFileName_)) {
			auto doBreak = stepOpcode();
			if(doBreak) {
				if(freezeOnBreakPoint_) {
					freezeOnBreakPoint_ = false;
					gui_->cbGui().selector().freezeState = true;
				}

				break;
			}
		}

		// We don't want to update the cursor position here since that would
		// make navigation impossible when refreshing every frame.
		updatePosition(false);
		rerun_ = false;
	}

	// Handle input
	if(spvm_.state->code_current && textedit_.mFocused) {
		ImGuiIO& io = ImGui::GetIO();
		auto shift = io.KeyShift;
		auto ctrl = io.KeyCtrl;
		auto alt = io.KeyAlt;

		auto breakKey = ImGuiKey_F9;
		auto stepKey = ImGuiKey_F10;

		if(!shift && !ctrl && !alt && ImGui::IsKeyPressed(breakKey, false)) {
			toggleBreakpoint();
		} else if(!shift && !ctrl && !alt && ImGui::IsKeyPressed(stepKey, false)) {
			stepLine();
		}

		io.WantCaptureKeyboard = true;
	}

	// Controls
	if(spvm_.state->code_current) {
		if(ImGui::Button("Step Opcode")) {
			auto doBreak = false;
			// silently execute all init instructions here first
			do {
				doBreak = stepOpcode();
			} while(spvm_.state->current_line < 0 && spvm_.state->code_current && !doBreak);
		}
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Execute a single SPIR-V opcode");
		}

		ImGui::SameLine();

		if(spvm_.state->code_current && ImGui::Button("Step Line")) {
			stepLine();
		}
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Execute the program until the current line changes");
		}

		ImGui::SameLine();

		if(ImGui::Button("Run")) {
			auto doBreak = false;
			while(spvm_.state->code_current && !doBreak) {
				// TODO: really do this here? kinda hacky
				if(isLastReturn()) {
					break;
				}

				doBreak = stepOpcode();
			}

			updatePosition(true);
		}
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Execute the program until it's finished "
				"or a breakpoint triggered");
		}


		ImGui::SameLine();

		// toggle breakpoint
		if(ImGui::Button("Breakpoint")) {
			toggleBreakpoint();
		}
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Toggles the breakpoint at the current file/line "
				"of the code editor");
		}

		ImGui::SameLine();

		if(ImGui::Button("Move Cursor")) {
			updatePosition(true);
		}
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Move the cursor in the editor to the "
				"current execution position");
		}

		// TODO: execute to cursor button/functionality
	} else {
		ImGui::Text("Finished");

		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Program execution has finished.");
		}
	}

	ImGui::SameLine();

	if(ImGui::Button("Reset")) {
		spvm_state_delete(spvm_.state);
		initState();
		updatePosition(true);
	}
	if(gui_->showHelp && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Reset execution to the beginning");
	}

	ImGui::SameLine();

	{
		auto disable = gui_->cbGui().selector().freezeState;
		if(disable) {
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.6f);
		}

		ImGui::Checkbox("Freeze on Breakpoint", &freezeOnBreakPoint_);
		if(gui_->showHelp && ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Automatically freeze state when hitting "
				"a breakpoint");
		}

		if(disable) {
			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		}
	}

	// shader view
	if(ImGui::BeginChild("ShaderDebugger", ImVec2(0, -200))) {
		ImGui::PushFont(gui_->monoFont);
		textedit_.Render("Shader");
		ImGui::PopFont();
	}

	ImGui::EndChild();

	// variable view
	if(ImGui::BeginChild("Views")) {
		if(ImGui::BeginTabBar("Tabs")) {
			if(ImGui::BeginTabItem("Inputs")) {
				drawInputsTab();
				ImGui::EndTabItem();
			}

			if(ImGui::BeginTabItem("Callstack")) {
				drawCallstackTab();
				ImGui::EndTabItem();
			}

			if(ImGui::BeginTabItem("Variables")) {
				drawVariablesTab();
				ImGui::EndTabItem();
			}

			if(ImGui::BeginTabItem("Breakpoints")) {
				drawBreakpointsTab();
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}
	}

	ImGui::EndChild();

	currLine_ = spvm_.state->current_line;
	if(spvm_.state->current_file) {
		currFileName_ = spvm_.state->current_file;
	}
}

Vec3ui ShaderDebugger::workgroupSize() const {
	// parse workgroup size
	// TODO: cache on load for compute shaders
	spc::SpecializationConstant wgsc[3];
	Vec3ui wgs;
	compiled_->get_work_group_size_specialization_constants(wgsc[0], wgsc[1], wgsc[2]);

	for(auto i = 0u; i < 3u; ++i) {
		if(wgsc[i].id) {
			wgs[i] = compiled_->evaluate_constant_u32(wgsc[i].id);
		} else {
			wgs[i] = compiled_->get_execution_mode_argument(spv::ExecutionModeLocalSize, i);
			dlg_assert(wgs[i] != 0);
		}
	}

	return wgs;
}

Vec3ui ShaderDebugger::numWorkgroups() const {
	// TODO: cache/compute only if needed
	auto* baseCmd = selection().command().back();
	Vec3ui numWGs {};
	if(auto* idcmd = commandCast<const DispatchIndirectCmd*>(baseCmd); idcmd) {
		auto hookState = selection().completedHookState();
		dlg_assert(hookState);
		auto& ic = hookState->indirectCopy;
		auto span = ic.data();
		auto ecmd = read<VkDispatchIndirectCommand>(span);
		numWGs = {ecmd.x, ecmd.y, ecmd.z};
	} else if(auto* dcmd = commandCast<const DispatchCmd*>(baseCmd); dcmd) {
		numWGs = {dcmd->groupsX, dcmd->groupsY, dcmd->groupsZ};
	} else if(auto* dcmd = commandCast<const DispatchBaseCmd*>(baseCmd); dcmd) {
		numWGs = {dcmd->groupsX, dcmd->groupsY, dcmd->groupsZ};
	} else {
		dlg_error("unreachable");
	}

	return numWGs;
}

void ShaderDebugger::loadBuiltin(const spc::BuiltInResource& builtin,
		span<const spvm_word> indices, span<spvm_member> dst) {
	// dlg_trace("spirv OpLoad of builtin {}", builtin.builtin);

	auto loadVecU = [&](const auto& vec) {
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

	auto loadScalarU = [&](const auto& val) {
		dlg_assert(dst.size() == 1u);
		dlg_assert(valueType(dst[0]) == spvm_value_type_int);
		dst[0].value.u = val;
	};

	auto wgs = workgroupSize();
	auto numWGs = numWorkgroups();

	switch(builtin.builtin) {
		case spv::BuiltInNumSubgroups: {
			// TODO: proper subgroup support
			loadScalarU(1u);
			break;
		}
		case spv::BuiltInNumWorkgroups: {
			loadVecU(numWGs);
			break;
		}
		case spv::BuiltInWorkgroupSize: {
			loadVecU(wgs);
			break;
		}
		case spv::BuiltInWorkgroupId: {
			Vec3ui id {0u, 0u, 0u};
			// floor by design
			id.x = globalInvocationID_.x / wgs.x;
			id.y = globalInvocationID_.y / wgs.y;
			id.z = globalInvocationID_.z / wgs.z;
			loadVecU(id);
			break;
		}
		case spv::BuiltInGlobalInvocationId: {
			loadVecU(globalInvocationID_);
			break;
		}
		case spv::BuiltInLocalInvocationId: {
			Vec3ui id {0u, 0u, 0u};
			id.x = globalInvocationID_.x % wgs.x;
			id.y = globalInvocationID_.y % wgs.y;
			id.z = globalInvocationID_.z % wgs.z;
			loadVecU(id);
			break;
		}
		case spv::BuiltInLocalInvocationIndex: {
			Vec3ui lid {0u, 0u, 0u};
			lid.x = globalInvocationID_.x % wgs.x;
			lid.y = globalInvocationID_.y % wgs.y;
			lid.z = globalInvocationID_.z % wgs.z;
			auto id =
				lid.z * wgs.y * wgs.x +
				lid.y * wgs.x +
				lid.x;
			loadScalarU(id);
			break;
		} default:
			dlg_error("Unhandled builtin: {}", builtin.builtin);
			break;
	}
}

unsigned ShaderDebugger::arrayLength(unsigned varID, span<const spvm_word> indices) {
	ZoneScoped;
	dlg_assert(compiled_);

	auto res = resource(*this->compiled_, varID);
	if(!res) {
		dlg_error("OpArrayLength of invalid/unknown var {}", varID);
		return 0;
	}

	dlg_assert(compiled_->has_decoration(res->id, spv::DecorationDescriptorSet) &&
				compiled_->has_decoration(res->id, spv::DecorationBinding));
	auto& spcTypeMaybeArrayed = compiled_->get_type(res->type_id);

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
			bounds = compiled_->evaluate_constant_u32(bounds);
		}

		dlg_assert(u32(indices[0]) < bounds);

		arrayElemID = indices[0];
		indices = indices.subspan(1u);
		spcType.array.clear();
		spcType.array_size_literal.clear();
	}

	auto setID = compiled_->get_decoration(res->id, spv::DecorationDescriptorSet);
	auto bindingID = compiled_->get_decoration(res->id, spv::DecorationBinding);

	auto* baseCmd = selection().command().back();
	auto* cmd = deriveCast<const StateCmdBase*>(baseCmd);
	auto& dsState = selection().descriptorSnapshot();
	auto dss = cmd->boundDescriptors().descriptorSets;

	dlg_assert(setID < dss.size());
	auto& cmdDS = dss[setID];

	auto stateIt = dsState.states.find(cmdDS.dsEntry);
	dlg_assert(stateIt != dsState.states.end());
	auto [ds, lock] = access(*stateIt->second);

	// For samplers, we didn't do a copy and so have to early-out here
	auto dsCopyIt = varIDToDsCopyMap_.find(varID);
	if(dsCopyIt == varIDToDsCopyMap_.end()) {
		dlg_error("OpArrayLength for var {} ERROR: not retrieved", varID);
		return 0;
	}

	auto hookState = selection().completedHookState();
	dlg_assert(hookState);
	// dsCopyIt->second stores the beginning of the range we stored in the
	// array elements in. So we have to offset it with the arrayElemID
	dlg_assert(hookState->copiedDescriptors.size() > dsCopyIt->second + arrayElemID);
	auto& copyResult = hookState->copiedDescriptors[dsCopyIt->second + arrayElemID];

	dlg_assert(copyResult.op.set == setID);
	dlg_assert(copyResult.op.binding == bindingID);

	dlg_assert_or(spcType.storage == spv::StorageClassStorageBuffer ||
		spcType.storage == spv::StorageClassUniform, return 0);

	auto buf = buffers(ds, bindingID)[arrayElemID];
	u32 size = 0u;
	if(buf.buffer) {
		size = evalRange(buf.buffer->ci.size, buf.offset, buf.range);
	} else {
		// NOTE: copied size will usually match the raw buffer size,
		// except it was truncated for some reason (e.g. because it
		// was too large).
		dlg_warn("buffer was destroyed, using copied size instead");
		auto* buf = std::get_if<OwnBuffer>(&copyResult.data);
		dlg_assert(buf);
		size = buf->data().size();
	}

	ThreadMemScope tms;
	auto [type, off] = accessBuffer(tms, res->type_id, indices, size);
	dlg_assert_or(type, return 0);

	dlg_assert(!type->array.empty());
	dlg_assert(type->array[0] == 0u);
	auto subSize = type->deco.arrayStride;
	for(auto dim : span(type->array).subspan(1)) {
		dlg_assert(dim != 0u); // only first dimension can be runtime size
		subSize *= dim;
	}

	dlg_assert(size % subSize == 0u);
	return size / subSize;
}

std::pair<const Type*, u32> ShaderDebugger::accessBuffer(ThreadMemScope& tms,
		unsigned typeID, span<const spvm_word> indices, u32 dataSize) {
	const auto* type = buildType(*compiled_, typeID, tms.customUse());
	dlg_assert(type);

	auto off = 0u;
	while(!indices.empty()) {
		if(!type->array.empty()) {
			auto count = type->array[0];
			auto remDims = span<u32>(type->array).subspan(1);

			auto subSize = type->deco.arrayStride;
			dlg_assert(subSize);

			for(auto size : remDims) {
				dlg_assert(size != 0u); // only first dimension can be runtime size
				subSize *= size;
			}

			if(count == 0u) { // runtime array, find out size
				auto remSize = dataSize - off;
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

			auto& cpy = tms.construct<Type>();
			cpy = *type;
			cpy.array = remDims;
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

			auto& cpy = tms.construct<Type>();
			cpy = *type;
			cpy.columns = 1u;
			type = &cpy;
		} else if(type->vecsize > 1) {
			auto id = u32(indices[0]);
			indices = indices.subspan(1);
			dlg_assert(id < type->vecsize);

			auto& cpy = tms.construct<Type>();
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
	return {type, u32(off)};
}

void ShaderDebugger::loadVar(unsigned srcID, span<const spvm_word> indices,
		span<spvm_member> dst, u32 typeID) {
	ZoneScoped;
	dlg_assert(compiled_);

	auto res = resource(*this->compiled_, srcID);
	if(!res) {
		auto builtin = builtinResource(*this->compiled_, srcID);
		if(!builtin) {
			dlg_error("OpLoad of invalid/unknown var {}", srcID);
			return;
		}

		loadBuiltin(*builtin, indices, dst);
		return;
	}

	// dlg_trace("spirv OpLoad of non-builtin var {}", srcID);

	dlg_assert(compiled_->has_decoration(res->id, spv::DecorationDescriptorSet) &&
				compiled_->has_decoration(res->id, spv::DecorationBinding));
	auto& spcTypeMaybeArrayed = compiled_->get_type(res->type_id);

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
			bounds = compiled_->evaluate_constant_u32(bounds);
		}

		dlg_assert(u32(indices[0]) < bounds);

		arrayElemID = indices[0];
		indices = indices.subspan(1u);
		spcType.array.clear();
		spcType.array_size_literal.clear();
	}

	auto setID = compiled_->get_decoration(res->id, spv::DecorationDescriptorSet);
	auto bindingID = compiled_->get_decoration(res->id, spv::DecorationBinding);

	auto* baseCmd = selection().command().back();
	auto* cmd = deriveCast<const StateCmdBase*>(baseCmd);
	auto& dsState = selection().descriptorSnapshot();
	auto dss = cmd->boundDescriptors().descriptorSets;

	dlg_assert(setID < dss.size());
	auto& cmdDS = dss[setID];

	auto stateIt = dsState.states.find(cmdDS.dsEntry);
	dlg_assert(stateIt != dsState.states.end());
	auto [ds, lock] = access(*stateIt->second);

	// For samplers, we didn't do a copy and so have to early-out here
	auto dsCopyIt = varIDToDsCopyMap_.find(srcID);
	if(dsCopyIt == varIDToDsCopyMap_.end()) {
		dlg_assert_or(spcType.basetype == spc::SPIRType::Sampler, return);
		// ugh, not sure which one is right here
		dlg_assert_or(spcType.storage == spv::StorageClassUniform || spcType.storage == spv::StorageClassUniformConstant, return);
		dlg_assert(indices.empty());

		// dlg_trace(" >> found sampler");

		auto image = vil::images(ds, bindingID)[arrayElemID];
		dlg_assert(image.sampler);

		auto& sampler = samplers_.emplace_back();
		sampler.desc = setupSampler(*image.sampler);

		dlg_assert(dst.size() == 1u);
		dst[0].value.sampler = &sampler;

		dlg_assert(dst[0].member_count == 0u);
		return;
	}

	auto hookState = selection().completedHookState();
	dlg_assert(hookState);
	// dsCopyIt->second stores the beginning of the range we stored in the
	// array elements in. So we have to offset it with the arrayElemID
	dlg_assert(hookState->copiedDescriptors.size() > dsCopyIt->second + arrayElemID);
	auto& copyResult = hookState->copiedDescriptors[dsCopyIt->second + arrayElemID];

	dlg_assert(copyResult.op.set == setID);
	dlg_assert(copyResult.op.binding == bindingID);
	dlg_assert(copyResult.op.elem == 0u);

	if(spcType.storage == spv::StorageClassPushConstant) {
		auto pcrData = cmd->boundPushConstants().data;

		ThreadMemScope tms;
		auto [type, off] = accessBuffer(tms, res->type_id, indices, pcrData.size());
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

		// NOTE: this can happen I guess, e.g. if not the whole range was
		// bound and the shader is reading undefined data I guess?
		auto typeSize = size(*type, BufferLayout::std140);
		auto end = off + typeSize;
		dlg_assert(end <= pcrData.size());
		if(end > pcrData.size()) {
			// read undefined data...
			auto undefData = tms.allocUndef<std::byte>(typeSize);
			if(off < pcrData.size()) {
				std::copy(pcrData.begin() + off, pcrData.end(), undefData.begin());
			}
			pcrData = undefData;
		} else {
			pcrData = pcrData.subspan(off);
		}

		setupMember(*type, pcrData, *setupDst);
		return;
	} else if(spcType.storage == spv::StorageClassInput) {
		// TODO: get from vertex input?
		dlg_error("TODO: loadVar input");
		return;
	} else if(spcType.storage == spv::StorageClassOutput) {
		// TODO: from own storage? not sure what this actually means
		dlg_error("TODO: loadVar output");
		return;
	} else if(spcType.storage == spv::StorageClassUniformConstant) {
		// we already handled samplers above
		dlg_assert(spcType.basetype != spc::SPIRType::Sampler);

		// TODO: imageView, image from descriptor might be null here.
		// We should probably just encode the needed information into
		// the CommandHook so we can read the data even if original
		// image/view were destroyed.
		if(spcType.basetype == spc::SPIRType::Image) {
			dlg_assert(dst.size() == 1u);

			auto image = vil::images(ds, bindingID)[arrayElemID];
			if(!image.imageView || !image.imageView->img) {
				dlg_warn("source imageView/image were destroyed");
				dst[0].value.image = const_cast<OurImage*>(&emptyImage);
				return;
			}

			auto& imgView = *image.imageView;
			auto& img = *imgView.img;

			auto* buf = std::get_if<CopiedImageToBuffer>(&copyResult.data);
			dlg_assert(buf);

			auto& dstImg = images_.emplace_back();
			dstImg.width = img.ci.extent.width;
			dstImg.height = img.ci.extent.height;
			dstImg.depth = img.ci.extent.depth;
			dstImg.levels = imgView.ci.subresourceRange.levelCount;
			dstImg.layers = imgView.ci.subresourceRange.layerCount;
			dstImg.data = buf->buffer.data();
			dstImg.format = buf->format;

			dst[0].value.image = &dstImg;
			dlg_assert(u32(dst[0].type) == res->base_type_id);
			dlg_assert(dst[0].member_count == 0u);

			return;
		} else if(spcType.basetype == spc::SPIRType::SampledImage) {
			dlg_assert(dst.size() == 2u);

			auto image = vil::images(ds, bindingID)[arrayElemID];
			if(!image.imageView || !image.imageView->img) {
				dlg_warn("source imageView/image or sampler were destroyed");
				dst[0].value.image = const_cast<OurImage*>(&emptyImage);
				dst[1].value.sampler = const_cast<spvm_sampler*>(&defaultSampler);
				return;
			}

			auto& imgView = *image.imageView;
			auto& img = *imgView.img;

			auto* buf = std::get_if<CopiedImageToBuffer>(&copyResult.data);
			dlg_assert(buf);

			auto& dstImg = images_.emplace_back();
			dstImg.width = img.ci.extent.width;
			dstImg.height = img.ci.extent.height;
			dstImg.depth = img.ci.extent.depth;
			dstImg.levels = imgView.ci.subresourceRange.levelCount;
			dstImg.layers = imgView.ci.subresourceRange.layerCount;
			dstImg.data = buf->buffer.data();
			dstImg.format = buf->format;

			auto& sampler = samplers_.emplace_back();
			sampler.desc = setupSampler(*image.sampler);

			auto& spvmRes = spvm_.state->results[srcID];
			auto* resType = spvm_state_get_type_info(spvm_.state->results, &spvm_.state->results[spvmRes.pointer]);

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
			auto* copiedBuf = std::get_if<OwnBuffer>(&copyResult.data);
			dlg_assert(copiedBuf);
			auto data = copiedBuf->data();

			auto buf = buffers(ds, bindingID)[arrayElemID];
			u32 size = 0u;
			if(buf.buffer) {
				size = evalRange(buf.buffer->ci.size, buf.offset, buf.range);
			} else {
				// NOTE: copied size will usually match the raw buffer size,
				// except it was truncated for some reason (e.g. because it
				// was too large).
				dlg_warn("buffer was destroyed, using copied size instead");
				size = data.size();
			}

			ThreadMemScope tms;
			auto [type, off] = accessBuffer(tms, res->type_id, indices, size);
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
	dlg_assert(compiled_);

	CommandHookOps ops {};
	varIDToDsCopyMap_.clear();

	auto resources = compiled_->get_shader_resources();

	auto addCopies = [&](auto& resources, bool imageAsBuffer) {
		for(auto& res : resources) {
			if(!compiled_->has_decoration(res.id, spv::DecorationDescriptorSet) ||
					!compiled_->has_decoration(res.id, spv::DecorationBinding)) {
				dlg_warn("resource {} doesn't have set/binding decorations", res.name);
				continue;
			}

			auto& type = compiled_->get_type_from_variable(res.id);
			varIDToDsCopyMap_.insert({u32(res.id), u32(ops.descriptorCopies.size())});

			DescriptorCopyOp dsCopy;
			dsCopy.set = compiled_->get_decoration(res.id, spv::DecorationDescriptorSet);
			dsCopy.binding = compiled_->get_decoration(res.id, spv::DecorationBinding);
			dsCopy.before = true;
			dsCopy.imageAsBuffer = imageAsBuffer;

			auto arraySize = 1u;
			if(type.array.size() > 1) {
				// TODO: support all array dimensions?
				dlg_error("multi-dim arrays not supported");
			}

			for(auto i = 0u; i < arraySize; ++i) {
				dsCopy.elem = i;
				ops.descriptorCopies.push_back(dsCopy);
			}
		}
	};

	addCopies(resources.sampled_images, true);
	addCopies(resources.separate_images, true);
	addCopies(resources.storage_images, true);
	addCopies(resources.storage_buffers, false);
	addCopies(resources.uniform_buffers, false);
	addCopies(resources.subpass_inputs, true);

	// for indirect dispatch, need to know the number of workgrups
	// since the shader might read that var
	// TODO: only do it if the shader accesses the variable?
	auto* baseCmd = selection().command().back();
	if(commandCast<const DispatchIndirectCmd*>(baseCmd) ||
			commandCast<const DrawIndirectCmd*>(baseCmd) ||
			commandCast<const DrawIndirectCountCmd*>(baseCmd) ||
			commandCast<const TraceRaysIndirectCmd*>(baseCmd)) {
		ops.copyIndirectCmd = true;
	}

	CommandHookUpdate update;
	update.invalidate = true;
	update.newOps = std::move(ops);
	hook.freeze.store(false);
	hook.updateHook(std::move(update));

	selection().clearState();
}

void ShaderDebugger::initVarMap() {
	dlg_assert(selection().completedHookState());
	auto& state = *selection().completedHookState();

	// TODO: support copied descriptor set without imageAsBuffer.
	// We can still manually download the data somehow

	varIDToDsCopyMap_.clear();
	auto resources = compiled_->get_shader_resources();

	auto initVars = [&](auto& resources, bool imageAsBuffer) {
		for(auto& res : resources) {
			if(!compiled_->has_decoration(res.id, spv::DecorationDescriptorSet) ||
					!compiled_->has_decoration(res.id, spv::DecorationBinding)) {
				dlg_warn("resource {} doesn't have set/binding decorations", res.name);
				continue;
			}

			auto& type = compiled_->get_type_from_variable(res.id);
			auto setID = compiled_->get_decoration(res.id, spv::DecorationDescriptorSet);
			auto bindingID = compiled_->get_decoration(res.id, spv::DecorationBinding);

			// don't have to insert for *each* elemID, they are sequential
			auto* copy = findDsCopy(state, setID, bindingID, 0u,
				true, imageAsBuffer);
			if(!copy) {
				dlg_warn("shader debugger: missing var");
				continue;
			}

			dlg_assert(copy >= state.copiedDescriptors.data());
			auto off = u32(copy - state.copiedDescriptors.data());
			dlg_assert(off < state.copiedDescriptors.size());
			varIDToDsCopyMap_.insert({u32(res.id), off});

			// assert that copies are indeed sequential
			dlg_check({
				auto arraySize = 1u;
				if(type.array.size() > 1) {
					// TODO: support all array dimensions?
					dlg_error("multi-dim arrays not supported");
				}

				for(auto elemID = 0u; elemID < arraySize; ++elemID) {
					auto* ecopy = findDsCopy(state, setID, bindingID, elemID,
						true, imageAsBuffer);
					dlg_assert(ecopy && (ecopy - copy) == i64(elemID));
				}
			});
		}
	};

	initVars(resources.sampled_images, true);
	initVars(resources.separate_images, true);
	initVars(resources.storage_images, true);
	initVars(resources.storage_buffers, false);
	initVars(resources.uniform_buffers, false);
	initVars(resources.subpass_inputs, true);
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
		} else if(type.width == 16) {
			dst.value.f = read<f16>(data);
		} else {
			dlg_error("Invalid width: float with {} bits", type.width);
		}
	} else if(type.type == Type::typeInt) {
		dlg_assert(vt == spvm_value_type_int);

		if(type.width == 64) {
			dst.value.s = read<i64>(data);
		} else if(type.width == 32) {
			dst.value.s = read<i32>(data);
		} else if(type.width == 16) {
			dst.value.s = read<i16>(data);
		} else if(type.width == 8) {
			dst.value.s = read<i8>(data);
		} else {
			dlg_error("Invalid width: int with {} bits", type.width);
		}
	} else if(type.type == Type::typeUint) {
		dlg_assert(vt == spvm_value_type_int);

		if(type.width == 64) {
			dst.value.u = read<u64>(data);
		} else if(type.width == 32) {
			dst.value.u = read<u32>(data);
		} else if(type.width == 16) {
			dst.value.u = read<u16>(data);
		} else if(type.width == 8) {
			dst.value.u = read<u8>(data);
		} else {
			dlg_error("Invalid width: uint with {} bits", type.width);
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
		setupScalar(nextType, data.subspan(i * stride), elems[i]);
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
			nt.array = {};
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

	auto fmtSize = FormatElementSize(img.format);
	off *= fmtSize;
	if(off + fmtSize > img.data.size()) {
		dlg_warn("Image read would be out of range: \n"
			"\tpos ({}, {}, {}), layer {}, level {}\n"
			"\timage data size in pixels: {}",
			x, y, z, layer, level, img.data.size() / fmtSize);
		return {};
	}

	auto texelBytes = img.data.subspan(off, fmtSize);
	auto texel = read(img.format, texelBytes);

	return {float(texel[0]), float(texel[1]), float(texel[2]), float(texel[3])};
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

spvm_value_type ShaderDebugger::valueType(const spvm_member& member) {
	auto* resType = spvm_state_get_type_info(spvm_.state->results, &spvm_.state->results[member.type]);
	dlg_assert(resType);
	return resType->value_type;
}

void ShaderDebugger::display(const char* name, const spvm_member& member) {
	ImGui::TableNextRow();
	ImGui::TableNextColumn();

	auto id = dlg::format("{}:{}", name, (void*) name);
	auto flags = ImGuiTreeNodeFlags_FramePadding;

	auto* ptype = spvm_state_get_type_info(spvm_.state->results, &spvm_.state->results[member.type]);
	dlg_assert(ptype);
	auto& type = *ptype;

	if(type.value_type == spvm_value_type_struct) {
		if(ImGui::TreeNodeEx(id.c_str(), flags, "%s", name)) {
			dlg_assert(type.member_count == member.member_count);
			for(auto i = 0u; i < u32(member.member_count); ++i) {
				std::string mname;
				if(type.member_name && i < u32(type.member_name_count)) {
					mname = type.member_name[i];
				} else {
					mname = dlg::format("?{}", i);
				}

				display(mname.c_str(), member.members[i]);
			}

			ImGui::TreePop();
		}

		return;
	} else if(type.value_type == spvm_value_type_array) {
		if(ImGui::TreeNodeEx(id.c_str(), flags, "%s", name)) {
			for(auto i = 0; i < member.member_count; ++i) {
				auto aname = dlg::format("[{}]", i);
				display(aname.c_str(), member.members[i]);
			}

			ImGui::TreePop();
		}

		return;
	} else if(type.value_type == spvm_value_type_matrix) {
		// TODO: have to take care since they are col-major
		imGuiText("TODO: matrix printing");
		return;
	}

	ImGui::AlignTextToFramePadding();
	ImGui::Bullet();
	ImGui::SameLine();
	imGuiText("{}", name);
	ImGui::TableNextColumn();

	if(type.value_type == spvm_value_type_vector) {
		auto str = std::string{};

		for(auto i = 0; i < member.member_count; ++i) {
			if(i != 0) {
				str += ", ";
			}
			str += formatScalar(member.members[i]);
		}

		ImGui::AlignTextToFramePadding();
		imGuiText("{}", str);
		return;
	}

	// Invalid types for values
	dlg_assert_or(type.value_type != spvm_value_type_void, return);
	dlg_assert_or(type.value_type != spvm_value_type_runtime_array, return);

	// TODO: might happen I guess
	dlg_assert_or(type.value_type != spvm_value_type_sampler, return);
	dlg_assert_or(type.value_type != spvm_value_type_image, return);
	dlg_assert_or(type.value_type != spvm_value_type_sampled_image, return);

	// Must be scalar
	auto fs = formatScalar(member);
	imGuiText("{}", fs);

	// TODO: tooltip
}

std::string ShaderDebugger::formatScalar(const spvm_member& member) {
	std::string str;
	auto vt = valueType(member);
	if(vt == spvm_value_type_int) {
		str += dlg::format("{}", member.value.s);
	} else if(vt == spvm_value_type_float) {
		str += dlg::format("{}", member.value.f);
	} else if(vt == spvm_value_type_bool) {
		str += dlg::format("{}", bool(member.value.b));
	} else {
		str += dlg::format("<Unable to format value type '{}'>", int(vt));
	}

	return str;
}

void ShaderDebugger::drawVariablesTab() {
	// imGuiText("Current line: {}", state->current_line);
	// imGuiText("Instruction count : {}", state->instruction_count);
	// imGuiText("Offset: {}", state->code_current - program->code);

	// vars_
	// for(auto [name, res] : vars_) {
	// 	printRes(name, res);
	// }

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, gui_->uiScale() * ImVec2(4.f, 2.5f));
	auto flags = ImGuiTableFlags_BordersInner |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_SizingStretchSame;
	if(ImGui::BeginTable("Values", 2u, flags)) {
		ImGui::TableSetupColumn(nullptr, 0, 0.25f);
		ImGui::TableSetupColumn(nullptr, 0, 0.75f);

		// local vars
		for(auto i = 0u; i < spvm_.program->bound; ++i) {
			auto& res = spvm_.state->results[i];
			if((res.type != spvm_result_type_variable && res.type != spvm_result_type_function_parameter) ||
					!res.name ||
					res.owner != spvm_.state->current_function ||
					// TODO: make this filter optional, via gui.
					// Useful in many cases but can be annoying/incorrect when
					// a variable isn't written on all branches.
					// To fix this (the filter setting/checkbox is probably
					// still a good idea) we could store the first OpStore
					// to this variable in opcode_setup of spvm. And then
					// here compare whether the current position of state
					// postdominates that instruction.
					!res.stored_to) {
				continue;
			}

			if(res.owner != spvm_.state->current_function) {
				continue;
			}

			// auto it = vars_.find(res.name);
			// if(it != vars_.end()) {
			// 	continue;
			// }

			const spvm_member* setupDst;
			spvm_member wrapper;
			auto* resType = spvm_state_get_type_info(spvm_.state->results,
				&spvm_.state->results[res.pointer]);
			auto vt = resType->value_type;

			if(vt == spvm_value_type_matrix ||
					vt == spvm_value_type_vector ||
					vt == spvm_value_type_struct ||
					vt == spvm_value_type_array) {
				setupDst = &wrapper;
				wrapper.type = u32(resType - spvm_.state->results); // type id
				wrapper.members = res.members;
				wrapper.member_count = res.member_count;
			} else {
				dlg_assert(res.member_count == 1u);
				setupDst = &res.members[0];
			}

			display(res.name, *setupDst);
		}

		ImGui::EndTable();
	}

	ImGui::PopStyleVar(1);
}

void ShaderDebugger::drawBreakpointsTab() {
	for(auto& bp : breakpoints_) {
		ImGui::Bullet();
		ImGui::SameLine();
		imGuiText("{}:{}", fileName(bp.fileID), bp.lineID);
	}
}

void ShaderDebugger::drawCallstackTab() {
	// TODO: does not belong here. Visual representation would be useful.
	imGuiText("Current line: {}", spvm_.state->current_line);

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, gui_->uiScale() * ImVec2(4.f, 2.5f));
	auto flags = ImGuiTableFlags_BordersInner |
		ImGuiTableFlags_Resizable |
		ImGuiTableFlags_SizingStretchSame;
	if(ImGui::BeginTable("Callstack", 3u, flags)) {
		// TODO:
		// - allow selecting entries and then showing the respective variables from threre
		// - show file, line, function (and maybe hide address by default)
		auto count = spvm_.state->function_stack_current + 1;
		for(auto i = 0; i < count; ++i) {
			ImGui::TableNextRow();

			// TODO: show line!

			ImGui::TableNextColumn();
			imGuiText("{}", i);

			ImGui::TableNextColumn();
			imGuiText("{}", static_cast<const void*>(spvm_.state->function_stack[i]));

			ImGui::TableNextColumn();
			auto name = spvm_.state->function_stack_info[i]->name;
			imGuiText("{}", name ? name : "?");
		}

		ImGui::EndTable();
	}

	ImGui::PopStyleVar(1);
}

void ShaderDebugger::drawInputsTab() {
	// TODO: decide depending on shader type
	using nytl::vec::cw::operators::operator*;

	auto wgs = workgroupSize();
	auto numWGs = numWorkgroups();
	auto numThreads = numWGs * wgs;

	if(numThreads.x * numThreads.y * numThreads.z == 0u) {
		imGuiText("This dispatch call has no invocations!");
	} else {
		ImGui::Checkbox("Allow out-of-bounds invocation", &allowSelectOutOfBounds_);
		auto sliderFlags = 0u;
		if(!allowSelectOutOfBounds_) {
			globalInvocationID_.x = std::min(globalInvocationID_.x, numThreads.x - 1);
			globalInvocationID_.y = std::min(globalInvocationID_.y, numThreads.y - 1);
			globalInvocationID_.z = std::min(globalInvocationID_.z, numThreads.z - 1);
			sliderFlags = ImGuiSliderFlags_AlwaysClamp;
		}

		// output global
		const unsigned globalDim =
			numThreads.z > 1 ? 3 :
			numThreads.y > 1 ? 2 :
			numThreads.x > 1 ? 1 : 0;
		float startX = 0.f;
		float sizeX = 100.f;
		if(globalDim == 0u && !allowSelectOutOfBounds_) {
			imGuiText(numThreads.x == 1 ?
				"There is only one invocation in this dispatch" :
				"There are no invocations in this dispatch");
		} else {
			imGuiText("GlobalInvocation:");
			ImGui::PushID("GlobalInv");

			for(auto i = 0u; i < globalDim; ++i) {
				ImGui::PushID(i);
				ImGui::SameLine();
				if(i == 0u) {
					startX = ImGui::GetCursorPosX();
					sizeX = ImGui::GetContentRegionAvail().x / float(globalDim) - 10.f;
					// sizeX = ImGui::GetContentRegionAvail().x / 3.f - 10.f;
				}

				ImGui::PushItemWidth(sizeX);
				int v = globalInvocationID_[i];
				ImGui::DragInt("", &v, 1.f, 0, numThreads[i] - 1, "%d", sliderFlags);
				globalInvocationID_[i] = u32(v);
				ImGui::PopID();
			}

			ImGui::PopID();
		}

		// output work group
		const unsigned wgDim =
			numWGs.z > 1 ? 3 :
			numWGs.y > 1 ? 2 :
			numWGs.x > 1 ? 1 : 0;
		if(wgDim > 0) {
			imGuiText("WorkGroup:");
			ImGui::PushID("WorkGroup");

			for(auto i = 0u; i < wgDim; ++i) {
				ImGui::PushID(i);
				ImGui::SameLine();
				if(i == 0u) {
					ImGui::SetCursorPosX(startX);
				}

				ImGui::PushItemWidth(sizeX);
				// floor by design
				auto before = globalInvocationID_[i] / wgs[i];
				int v = before;
				if(ImGui::DragInt("", &v, 1.f, 0, numWGs[i] - 1, "%d", sliderFlags)) {
					globalInvocationID_[i] += (v - before) * wgs[i];
				}
				ImGui::PopID();
			}

			ImGui::PopID();
		}

		// output local invocation
		const unsigned localDim =
			wgs.z > 1 ? 3 :
			wgs.y > 1 ? 2 :
			wgs.x > 1 ? 1 : 0;
		if(localDim > 0) {
			imGuiText("LocalInvocation:");
			ImGui::PushID("LocalInv");

			for(auto i = 0u; i < localDim; ++i) {
				ImGui::PushID(i);
				ImGui::SameLine();
				if(i == 0u) {
					ImGui::SetCursorPosX(startX);
				}

				ImGui::PushItemWidth(sizeX);
				// floor by design
				auto before = globalInvocationID_[i] % wgs[i];
				int v = before;
				if(ImGui::DragInt("", &v, 1.f, 0, wgs[i] - 1, "%d", sliderFlags)) {
					globalInvocationID_[i] += v - before;
				}
				ImGui::PopID();
			}

			ImGui::PopID();
		}
	}
}

void ShaderDebugger::updatePosition(bool moveCursor) {
	if(!spvm_.state->current_file) {
		dlg_warn("Can't jump to debugging state, current_file is null");
		return;
	}

	if(spvm_.state->current_line < 0) {
		dlg_warn("Can't jump to debugging state, current_line = {}",
			spvm_.state->current_line);
		return;
	}

	auto fileID = -1;
	for(auto i = 0u; i < spvm_.state->owner->file_count; ++i) {
		if(std::strcmp(spvm_.state->owner->files[i].name, spvm_.state->current_file) == 0u) {
			fileID = i;
			break;
		}
	}

	if(fileID == -1) {
		dlg_warn("Can't jump to debugging state, invalid file '{}'", spvm_.state->current_file);
		return;
	}

	if(currFileName_ != spvm_.state->owner->files[fileID].name) {
		dlg_trace("now in file {}", spvm_.state->owner->files[fileID].name);
		textedit_.SetText(spvm_.state->owner->files[fileID].source);
	}

	// TODO: seems to be a textedit bug
	auto line = spvm_.state->current_line == 0 ? 0 : spvm_.state->current_line - 1;
	textedit_.mCurrentLineNumber = {unsigned(line)};

	if(moveCursor) {
		textedit_.SetCursorPosition({line, 1});
	}
}

bool ShaderDebugger::stepOpcode() {
	auto currLine = spvm_.state->current_line;
	auto currFile = spvm_.state->current_file;

	auto posPrev = spvm_.state->code_current;
	spvm_state_step_opcode(spvm_.state);

	if(spvm_.state->current_line == currLine &&
			spvm_.state->current_file == currFile) {
		// position hasn't changed, can't be a breakpoint
		return false;
	}

	for(auto& bp : breakpoints_) {
		// if(spvm_word(bp.lineID) == spvm_.state->current_line &&
		// 		fileName(bp.fileID) == spvm_.state->current_file) {
		// 	return true;
		// }
		if(posPrev == bp.pos) {
			return true;
		}
	}

	return false;
}

std::string_view ShaderDebugger::fileName(u32 fileID) const {
	dlg_assert_or(fileID < spvm_.state->owner->file_count, return "");
	return spvm_.state->owner->files[fileID].name;
}

u32 ShaderDebugger::fileID(std::string_view fileName) const {
	auto fileID = u32(-1);
	for(auto i = 0u; i < spvm_.state->owner->file_count; ++i) {
		if(std::strncmp(spvm_.state->owner->files[i].name,
				fileName.data(), fileName.size()) == 0u) {
			fileID = i;
			break;
		}
	}

	dlg_assertm(fileID != u32(-1), "Can't find file {}", fileName);
	return fileID;

}

void ShaderDebugger::toggleBreakpoint() {
	// TODO: get current editor file
	auto fileName = spvm_.state->current_file;
	if(!fileName) {
		dlg_assert(spvm_.state->owner->file_count > 0);
		fileName = spvm_.state->owner->files[0].name;
	}

	const auto fid = fileID(fileName);
	const auto line = 1 + u32(textedit_.GetCursorPosition().mLine);
	bool doSet = true;

	for(auto it = breakpoints_.begin(); it != breakpoints_.end(); ++it) {
		auto& bp = *it;
		if(bp.lineID == line && bp.fileID == fid) {
			doSet = false;
			breakpoints_.erase(it);
			// TODO: file
			textedit_.GetBreakpoints().erase(line);
			break;
		}
	}

	if(doSet) {
		// find position
		auto spv = spvm_.program->code;
		auto spvEnd = spvm_.program->code + spvm_.program->code_length;

		struct Find {
			const spvm_source pos;
			u32 line;
		};

		std::optional<Find> best;

		while(spv < spvEnd) {
			auto instrBegin = spv;
			spvm_word opcode_data = SPVM_READ_WORD(spv);
			spvm_word word_count = ((opcode_data & (~SpvOpCodeMask)) >> SpvWordCountShift);
			SpvOp opcode = SpvOp(opcode_data & SpvOpCodeMask);

			if(opcode == SpvOpLine) {
				u32 opfile = SPVM_READ_WORD(spv);
				u32 opline = SPVM_READ_WORD(spv);
				u32 opclmn = SPVM_READ_WORD(spv);
				(void) opclmn;

				auto opfileID = fileID(spvm_.state->results[opfile].name);
				if(opfileID == fid && opline >= line) {
					if(!best || opline < best->line) {
						best.emplace(Find{instrBegin, opline});

						if(opline == line) {
							// exact
							break;
						}
					}
				}
			}

			dlg_assert(word_count > 0);
			spv = instrBegin + word_count;
		}

		// TODO: add threshold? we currently might add a breakpoint
		// somewhere else, just because it's the closest.
		// Should at least be in the same function, right?
		if(!best) {
			dlg_error("Coun't find location for breakpoint");
			return;
		}

		// TODO: move visual presentation of breakpoint to the
		// exact found position?

		auto& nbp = breakpoints_.emplace_back();
		nbp.fileID = fid;
		nbp.lineID = line;
		nbp.pos = best->pos;
		textedit_.GetBreakpoints().insert(line);
	}
}

void ShaderDebugger::stepLine() {
	auto doBreak = false;
	auto line = spvm_.state->current_line;
	// TODO: should probably also check for file change here, right?
	// just in case we jump to a function in another file that
	// happens to be at the same line
	while(spvm_.state->current_line == line && spvm_.state->code_current && !doBreak) {
		doBreak = stepOpcode();
	}

	updatePosition(true);
}

CommandSelection& ShaderDebugger::selection() const {
	return gui_->cbGui().selector();
}

} // namespace vil
