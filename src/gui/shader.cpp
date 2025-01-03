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
#include <vkutil/enumString.hpp>
#include <vk/format_utils.h>
#include <imgui/imgui_internal.h>
#include <vil_api.h>
#include <spirv_cross.hpp>

namespace vil {

using nytl::read;

ShaderDebugger::ShaderDebugger() = default;

ShaderDebugger::~ShaderDebugger() {
	unselect();
}

void ShaderDebugger::init(Gui& gui) {
	this->gui_ = &gui;

	// TODO: decide dynamically whether to use GLSL or HLSL here.
	// Can probalby parse it from the shader module
	const auto& lang = igt::TextEditor::LanguageDefinition::GLSLFast();
	textedit_.SetLanguageDefinition(lang);

	textedit_.SetShowWhitespaces(false);
	textedit_.SetTabSize(4);
	textedit_.SetReadOnly(true);
}

void ShaderDebugger::select(const Pipeline& pipe) {
	if(pipe.type == VK_PIPELINE_BIND_POINT_COMPUTE) {
		auto& cpipe = static_cast<const ComputePipeline&>(pipe);
		auto mod = copySpecializeSpirv(cpipe.stage);
		select(VK_SHADER_STAGE_COMPUTE_BIT, 0,
			std::move(mod), cpipe.stage.entryPoint);
	} else if(pipe.type == VK_PIPELINE_BIND_POINT_GRAPHICS) {
		auto& gpipe = static_cast<const GraphicsPipeline&>(pipe);
		u32 fragmentID = u32(-1);
		u32 vertexID = u32(-1);
		for(auto [i, stage] : enumerate(gpipe.stages)) {
			if(stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
				vertexID = i;
			} else if(stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
				fragmentID = i;
			}
		}

		if(vertexID != u32(-1)) {
			auto& vertex = gpipe.stages[vertexID];
			auto mod = copySpecializeSpirv(vertex);
			select(vertex.stage, vertexID, std::move(mod), vertex.entryPoint);
		} else if(fragmentID != u32(-1)) {
			auto& fragment = gpipe.stages[fragmentID];
			auto mod = copySpecializeSpirv(fragment);
			select(fragment.stage, fragmentID, std::move(mod), fragment.entryPoint);
		} else {
			dlg_error("No supported debugging stage in pipeline");
			unselect();
		}
	} else if(pipe.type == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
		auto& rpipe = static_cast<const RayTracingPipeline&>(pipe);
		const PipelineShaderStage* raygen {};
		auto stageID = 0u;
		for(auto [i, stage] : enumerate(rpipe.stages)) {
			if(stage.stage == VK_SHADER_STAGE_RAYGEN_BIT_KHR) {
				raygen = &stage;
				stageID = i;
				break;
			}
		}

		if(!raygen) {
			dlg_error("No raygen shader in raytracing pipe?!");
			unselect();
			return;
		}

		auto mod = copySpecializeSpirv(*raygen);
		select(raygen->stage, stageID, std::move(mod), raygen->entryPoint);
	}
}

void ShaderDebugger::select(VkShaderStageFlagBits stage, u32 stageID,
		std::unique_ptr<spc::Compiler> compiled,
		std::string entryPoint) {
	unselect();

	this->stage_ = stage;
	this->stageID_ = stageID;
	this->compiled_ = std::move(compiled);
	this->stage_ = stage;
	this->entryPoint_ = std::move(entryPoint);

	// find first valid file
	auto& ir = compiled_->get_ir();
	for(auto [i, source] : enumerate(ir.sources)) {
		// e.g. bad (and duplicated) OpSource instructions by slang
		if(source.source.empty()) {
			continue;
		}

		sourceFilesIDs_.push_back(i);
	}

	if(sourceFilesIDs_.empty()) {
		textedit_.SetText("<No debug code embedded in spirv>");
	} else {
		currentFile_ = sourceFilesIDs_[0];
		textedit_.SetText(fileContent(currentFile_));
	}
}

void ShaderDebugger::unselect() {
	compiled_ = {};
	stage_ = {};
	stageID_ = {};
	breakpoints_.clear();
	currentFile_ = 0u;
	sourceFilesIDs_.clear();
	entryPoint_ = {};
	invocationID_ = {};

	patch_.reset(gui_->dev());
}

void ShaderDebugger::draw() {
	if(!compiled_) {
		imGuiText("No shader selected for debugging");
		return;
	}

	if(sourceFilesIDs_.empty()) {
		// TODO: allow to show and debug plain spirv without debug sources?
		imGuiText("The shader contains no debug sources");
		return;
	}

	auto* baseCmd = selection().command().back();
	auto* stateCmd = deriveCast<const StateCmdBase*>(baseCmd);
	dlg_assert(stateCmd->boundPipe());

	if(stateCmd->category() == CommandCategory::draw) {
		auto* pipe = static_cast<const GraphicsPipeline*>(stateCmd->boundPipe());

		if(ImGui::BeginCombo("Shader Stage", vk::name(stage_))) {
			for(auto [i, stage] : enumerate(pipe->stages)) {
				if(stage.stage != VK_SHADER_STAGE_VERTEX_BIT &&
						stage.stage != VK_SHADER_STAGE_FRAGMENT_BIT) {
					dlg_warn("shader stage {} unsupported for debugging",
						vk::name(stage.stage));
					continue;
				}

				auto str = dlg::format("{}: {}", vk::name(stage.stage), stage.entryPoint);
				if(ImGui::Selectable(str.c_str())) {
					auto mod = copySpecializeSpirv(stage);
					select(stage.stage, i, std::move(mod), stage.entryPoint);
					break;
				}
			}
			ImGui::EndCombo();
		}
	} else if(stateCmd->category() == CommandCategory::traceRays) {
		auto* pipe = static_cast<const RayTracingPipeline*>(stateCmd->boundPipe());

		if(ImGui::BeginCombo("Shader Stage", vk::name(stage_))) {
			for(auto [i, stage] : enumerate(pipe->stages)) {
				auto str = dlg::format("{}: {}", vk::name(stage.stage), stage.entryPoint);
				if(ImGui::Selectable(str.c_str())) {
					auto mod = copySpecializeSpirv(stage);
					select(stage.stage, i, std::move(mod), stage.entryPoint);
					break;
				}
			}
			ImGui::EndCombo();
		}
	}

	if(ImGui::BeginCombo("File", fileName(currentFile_).c_str())) {
		for(auto& id : sourceFilesIDs_) {
			if(ImGui::Selectable(fileName(id).c_str())) {
				currentFile_ = id;
				textedit_.SetText(fileContent(currentFile_));
			}
		}

		ImGui::EndCombo();
	}

	// shader view
	if(ImGui::BeginChild("ShaderDebugger", ImVec2(0, -200))) {
		ImGui::PushFont(gui_->monoFont);
		textedit_.Render("Shader");
		ImGui::PopFont();
	}

	if(livePatch_) {
		auto& dev = gui_->dev();
		if(patch_.updateJobs(dev)) {
			updateHooks(*dev.commandHook);
		}

		auto currLine = 1 + u32(textedit_.GetCursorPosition().mLine);
		if(currLine != patch_.line || currentFile_ != patch_.file) {
			patch_.reset(dev);

			patch_.line = currLine;
			patch_.file = currentFile_;


			auto& job = patch_.jobs.emplace_back();
			job.data = std::make_unique<PatchJobData>();
			job.data->pipe.reset(stateCmd->boundPipe());
			job.data->compiler = compiled_.get();
			job.data->entryPoint = entryPoint_;
			job.data->stage = stage_;
			job.data->stageID = stageID_;
			job.data->file = patch_.file;
			job.data->line = patch_.line;
			job.data->captureAddress = dev.commandHook->shaderCaptureAddress();

			dlg_debug("Starting new patch job for {}:{}", patch_.file, patch_.line);
			job.result = std::async(std::launch::async,
				[&data = *job.data]{ return patchJob(data); });
		}
	}

	ImGui::EndChild();

	auto prevID = invocationID_;
	if(stage_ == VK_SHADER_STAGE_FRAGMENT_BIT) {
		auto& io = ImGui::GetIO();
		if(fragmentMode_ == FragmentMode::cursor) {
			invocationID_.x = io.MousePos[0];
			invocationID_.y = io.MousePos[1];
		} else if(fragmentMode_ == FragmentMode::cursorClicked &&
				io.MouseClicked[1]) {
			invocationID_.x = io.MousePos[0];
			invocationID_.y = io.MousePos[1];
		}
	}

	// variable view
	if(ImGui::BeginChild("Views")) {
		if(ImGui::BeginTabBar("Tabs")) {
			if(ImGui::BeginTabItem("Inputs")) {
				drawInputsTab();
				ImGui::EndTabItem();
			}
			if(ImGui::BeginTabItem("Variables")) {
				auto state = selection().completedHookState().get();
				if(state && state->shaderCapture.size && !patch_.current.captures.empty()) {
					Type base {};
					base.type = Type::typeStruct;
					base.members = patch_.current.captures;
					auto data = state->shaderCapture.data();
					auto io = read<Vec4u32>(data);

					imGuiText("hitcount for {}, {}, {}: {}",
						io[0], io[1], io[2], io[3]);

					if(io[3] >= 1u) {
						displayTable("vars", base, data);
					}
				}
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}
	}

	auto changed = prevID != invocationID_;
	if(changed && patch_.current.hook) {
		updateHooks(*gui_->dev().commandHook);
	}

	ImGui::EndChild();
}

Vec3ui ShaderDebugger::workgroupSize() const {
	dlg_assert_or(stage_ == VK_SHADER_STAGE_COMPUTE_BIT, return {});

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
	dlg_assert_or(stage_ == VK_SHADER_STAGE_COMPUTE_BIT, return {});

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

ShaderDebugger::DrawInfo ShaderDebugger::drawInfo() const {
	dlg_assert_or(stage_ == VK_SHADER_STAGE_VERTEX_BIT, return {});

	auto* baseCmd = selection().command().back();
	if(auto* idcmd = dynamic_cast<const DrawIndirectCmd*>(baseCmd); idcmd) {
		return {idcmd->drawCount, idcmd->indexed};
	} else if(auto* idcmd = dynamic_cast<const DrawIndirectCountCmd*>(baseCmd); idcmd) {
		auto hookState = selection().completedHookState();
		return {hookState->indirectCommandCount, idcmd->indexed};
	} else if(auto* dcmd = dynamic_cast<const DrawMultiCmd*>(baseCmd); dcmd) {
		return {u32(dcmd->vertexInfos.size()), false};
	} else if(auto* dcmd = dynamic_cast<const DrawMultiIndexedCmd*>(baseCmd); dcmd) {
		return {u32(dcmd->indexInfos.size()), true};
	}

	return {1u, static_cast<const DrawCmdBase*>(baseCmd)->isIndexed()};
}

ShaderDebugger::DrawCmdInfo ShaderDebugger::drawCmdInfo(u32 cmd) const {
	dlg_assert_or(stage_ == VK_SHADER_STAGE_VERTEX_BIT, return {});

	auto* baseCmd = selection().command().back();
	auto hookState = selection().completedHookState();

	auto readIndirect = [&](bool indexed, u32 numCmds) {
		auto& ic = hookState->indirectCopy;
		auto span = ic.data();

		dlg_assert(cmd < numCmds);
		if(indexed) {
			span = span.subspan(cmd * sizeof(VkDrawIndexedIndirectCommand));
			auto ecmd = read<VkDrawIndexedIndirectCommand>(span);
			return DrawCmdInfo{
				ecmd.vertexOffset,
				ecmd.indexCount,
				ecmd.instanceCount,
				ecmd.firstInstance,
				ecmd.firstIndex,
			};
		} else {
			span = span.subspan(cmd * sizeof(VkDrawIndirectCommand));
			auto ecmd = read<VkDrawIndirectCommand>(span);
			return DrawCmdInfo{
				i32(ecmd.firstVertex),
				ecmd.vertexCount,
				ecmd.instanceCount,
				ecmd.firstInstance,
			};
		}
	};

	if(auto* idcmd = dynamic_cast<const DrawIndirectCmd*>(baseCmd); idcmd) {
		return readIndirect(idcmd->indexed, idcmd->drawCount);
	} else if(auto* idcmd = dynamic_cast<const DrawIndirectCountCmd*>(baseCmd); idcmd) {
		dlg_assert(hookState);
		return readIndirect(idcmd->indexed, hookState->indirectCommandCount);
	} else if(auto* dcmd = dynamic_cast<const DrawCmd*>(baseCmd); dcmd) {
		return {
			i32(dcmd->firstVertex),
			dcmd->vertexCount,
			dcmd->instanceCount,
			dcmd->firstInstance,
		};
	} else if(auto* dcmd = dynamic_cast<const DrawIndexedCmd*>(baseCmd); dcmd) {
		return DrawCmdInfo{
			dcmd->vertexOffset,
			dcmd->indexCount,
			dcmd->instanceCount,
			dcmd->firstInstance,
			dcmd->firstIndex,
		};
	} else if(auto* dcmd = dynamic_cast<const DrawMultiCmd*>(baseCmd); dcmd) {
		dlg_assert(cmd < dcmd->vertexInfos.size());
		return DrawCmdInfo{
			i32(dcmd->vertexInfos[cmd].firstVertex),
			dcmd->vertexInfos[cmd].vertexCount,
			dcmd->instanceCount,
			dcmd->firstInstance,
		};
	} else if(auto* dcmd = dynamic_cast<const DrawMultiIndexedCmd*>(baseCmd); dcmd) {
		dlg_assert(cmd < dcmd->indexInfos.size());
		return DrawCmdInfo{
			dcmd->indexInfos[cmd].vertexOffset,
			dcmd->indexInfos[cmd].indexCount,
			dcmd->instanceCount,
			dcmd->firstInstance,
			dcmd->indexInfos[cmd].firstIndex,
		};
	}

	dlg_error("unreachable");
	return {};
}

const std::string& ShaderDebugger::fileName(u32 fileID) const {
	dlg_assert(compiled_);

	auto& ir = compiled_->get_ir();
	dlg_assert(fileID < ir.sources.size());

	auto& source = ir.sources[fileID];
	if(source.fileID == 0u) {
		static const auto empty = std::string {"unnamed"};
		return empty;
	}

	auto& str = ir.get<spc::SPIRString>(source.fileID);
	return str.str;
}

const std::string& ShaderDebugger::fileContent(u32 fileID) const {
	dlg_assert(compiled_);

	auto& ir = compiled_->get_ir();
	dlg_assert(fileID < ir.sources.size());

	auto& source = ir.sources[fileID];
	dlg_assert(!source.source.empty());
	return source.source;
}

CommandSelection& ShaderDebugger::selection() const {
	return gui_->cbGui().selector();
}

void ShaderDebugger::drawInputsTab() {
	if(stage_ == VK_SHADER_STAGE_COMPUTE_BIT) {
		drawInputsCompute();
	} else if(stage_ == VK_SHADER_STAGE_VERTEX_BIT) {
		drawInputsVertex();
	} else if(stage_ == VK_SHADER_STAGE_FRAGMENT_BIT) {
		drawInputsFragment();
	} else if(stage_ == VK_SHADER_STAGE_RAYGEN_BIT_KHR ||
			stage_ == VK_SHADER_STAGE_MISS_BIT_KHR ||
			stage_ == VK_SHADER_STAGE_ANY_HIT_BIT_KHR ||
			stage_ == VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR ||
			stage_ == VK_SHADER_STAGE_INTERSECTION_BIT_KHR ||
			stage_ == VK_SHADER_STAGE_CALLABLE_BIT_KHR) {
		drawInputsRaytrace();
	} else {
		dlg_error("Unsupported stage: {}", stage_);
		imGuiText("Error: unsupported stage selected");
	}
}

const char* ShaderDebugger::name(FragmentMode mode) {
	switch(mode) {
		case FragmentMode::cursor:
			return "Cursor";
		case FragmentMode::cursorClicked:
			return "Clicked Cursor";
		case FragmentMode::none:
			return "None";
		case FragmentMode::count:
			dlg_error("unreachable");
			return "<error>";
	}

	return "<error>";
}

void ShaderDebugger::drawInputsFragment() {
	// TODO: only show this when rendering via overlay, does not
	// make sense in separate window. We do not have this info
	// here atm tho

	if(ImGui::BeginCombo("Debug From", name(fragmentMode_))) {
		for(auto mode = u32(FragmentMode::none); mode < u32(FragmentMode::count); ++mode) {
			if(ImGui::Selectable(name(FragmentMode(mode)))) {
				fragmentMode_ = FragmentMode(mode);
			}
		}
		ImGui::EndCombo();
	}

	ImGui::DragInt2("Position", (int*) &invocationID_);
}

void ShaderDebugger::drawInputsVertex() {
	auto& commandID = invocationID_[0];
	auto& instanceID = invocationID_[1];
	auto& vertexID = invocationID_[2];

	auto [numCmds, indexed] = drawInfo();
	auto sliderFlags = 0u;

	// commandID
	if(numCmds > 1) {
		if(gui_->dev().shaderDrawParameters) {
			auto v = int(commandID);
			ImGui::DragInt("Command", &v, 1.f, 0, numCmds - 1, "%d", sliderFlags);
			commandID = v;
		} else {
			commandID = 0u;
			dlg_error("Multiple draw commands but device does not have "
				"ShaderParameters capability");
		}
	}
	commandID = std::min(commandID, numCmds - 1);

	// instanceID
	auto drawCmd = drawCmdInfo(commandID);
	if(drawCmd.numInis > 1) {
		auto v = int(instanceID);
		ImGui::DragInt("Instance", &v, 1.f, drawCmd.firstIni,
			drawCmd.firstIni + drawCmd.numInis - 1, "%d", sliderFlags);
		instanceID = v;
	}
	instanceID = std::min(instanceID, drawCmd.numInis - 1);

	// vertexID
	auto v = int(vertexID);
	auto minVertID = 0u;
	auto maxVertID = 0x7FFFFFFFu;
	if(!indexed) {
		minVertID = drawCmd.vertexOffset;
		maxVertID = minVertID + drawCmd.numVerts;
	}
	ImGui::DragInt("ID", &v, 1.f, minVertID, maxVertID - 1, "%d",
		ImGuiSliderFlags_AlwaysClamp);
	vertexID = v;
}

void ShaderDebugger::drawInputsCompute() {
	using nytl::vec::cw::operators::operator*;

	auto wgs = workgroupSize();
	auto numWGs = numWorkgroups();
	auto numThreads = numWGs * wgs;
	bool changed = false;

	if(numThreads.x * numThreads.y * numThreads.z == 0u) {
		imGuiText("This dispatch call has no invocations!");
	} else {
		auto sliderFlags = 0u;
		invocationID_.x = std::min(invocationID_.x, numThreads.x - 1);
		invocationID_.y = std::min(invocationID_.y, numThreads.y - 1);
		invocationID_.z = std::min(invocationID_.z, numThreads.z - 1);
		sliderFlags = ImGuiSliderFlags_AlwaysClamp;

		// output global
		const unsigned globalDim =
			numThreads.z > 1 ? 3 :
			numThreads.y > 1 ? 2 :
			numThreads.x > 1 ? 1 : 0;
		float startX = 0.f;
		float sizeX = 100.f;
		if(globalDim == 0u) {
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
				int v = invocationID_[i];
				changed |= ImGui::DragInt("", &v, 1.f, 0, numThreads[i] - 1, "%d", sliderFlags);
				invocationID_[i] = u32(v);
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
				auto before = invocationID_[i] / wgs[i];
				int v = before;
				if(ImGui::DragInt("", &v, 1.f, 0, numWGs[i] - 1, "%d", sliderFlags)) {
					invocationID_[i] += (v - before) * wgs[i];
					changed = true;
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
				auto before = invocationID_[i] % wgs[i];
				int v = before;
				if(ImGui::DragInt("", &v, 1.f, 0, wgs[i] - 1, "%d", sliderFlags)) {
					invocationID_[i] += v - before;
					changed = true;
				}
				ImGui::PopID();
			}

			ImGui::PopID();
		}
	}

	(void) changed;
}

void ShaderDebugger::drawInputsRaytrace() {
	imGuiText("Thread:");
	ImGui::PushID("GlobalInv");

	float sizeX;
	auto sliderFlags = ImGuiSliderFlags_AlwaysClamp;

	auto* baseCmd = selection().command().back();
	auto* rtCmd = deriveCast<const TraceRaysCmd*>(baseCmd);

	auto numThreads = Vec3ui {
		rtCmd->width,
		rtCmd->height,
		rtCmd->depth,
	};
	const unsigned globalDim =
		numThreads.z > 1 ? 3 :
		numThreads.y > 1 ? 2 :
		numThreads.x > 1 ? 1 : 0;

	for(auto i = 0u; i < globalDim; ++i) {
		ImGui::PushID(i);
		ImGui::SameLine();
		if(i == 0u) {
			sizeX = ImGui::GetContentRegionAvail().x / float(globalDim) - 10.f;
		}

		ImGui::PushItemWidth(sizeX);
		int v = invocationID_[i];
		ImGui::DragInt("", &v, 1.f, 0, numThreads[i] - 1, "%d", sliderFlags);
		invocationID_[i] = u32(v);
		ImGui::PopID();
	}

	ImGui::PopID();
}

void ShaderDebugger::updateHooks(CommandHook& hook) {
	CommandHookUpdate hookUpdate;
	hookUpdate.invalidate = true;
	auto& ops = hookUpdate.newOps.emplace();
	ops.shaderCapture = patch_.current.hook;
	ops.shaderCaptureInput = Vec3u32(invocationID_);
	hook.updateHook(std::move(hookUpdate));
}

bool ShaderDebugPatch::updateJobs(Device& dev) {
	auto finished = false;
	std::lock_guard lock(dev.mutex);

	for(auto it = jobs.begin(); it != jobs.end();) {
		auto& job = *it;
		auto res = job.result.wait_for(std::chrono::seconds(0));
		if(res != std::future_status::ready) {
			++it;
			continue;
		}

		if(job.data->state == PatchJobState::done) {
			auto res = job.result.get();
			if(res.error.empty()) {
				dlg_debug("patch job has finished succesfully. {} captures",
					res.captures.size());
				finished = true;
			} else {
				dlg_error("patch job error: {}", res.error);
			}

			current = std::move(res);
		}

		it = jobs.erase(it);
	}

	return finished;
}

void ShaderDebugPatch::reset(Device& dev) {
	for(auto& job : jobs) {
		std::lock_guard lock(dev.mutex);
		job.data->state = PatchJobState::canceled;
	}

	current = {};
	file = u32(-1);
	line = u32(-1);
}

} // namespace vil
