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

	// emu_ = std::make_unique<ShaderEmulation>(*this);
	if(emu_) {
		emu_->init();
	}
}

void ShaderDebugger::select(VkShaderStageFlagBits stage,
		std::unique_ptr<spc::Compiler> compiled,
		std::string entryPoint) {
	unselect();

	this->stage_ = stage;
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

	if(emu_) {
		emu_->select();
	}
}

void ShaderDebugger::unselect() {
	compiled_ = {};
	stage_ = {};
	breakpoints_.clear();
	currentFile_ = 0u;
	sourceFilesIDs_.clear();
	entryPoint_ = {};

	globalInvocationID_ = {};
	commandID_ = {};
	vertexID_ = {};
	instanceID_ = {};

	if(emu_) {
		emu_->unselect();
	}
}

void ShaderDebugger::draw() {
	if(!compiled_) {
		imGuiText("No shader selected for debugging");
		return;
	}

	auto hookState = selection().completedHookState();

	if(!hookState) {
		imGuiText("Waiting for submission using the shader...");
		return;
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
		patch_.updateJobs(dev);

		auto currLine = 1 + u32(textedit_.GetCursorPosition().mLine);
		if(currLine != patch_.line || currentFile_ != patch_.file) {
			patch_.reset(dev);

			patch_.line = currLine;
			patch_.file = currentFile_;

			auto* baseCmd = selection().command().back();
			auto* stateCmd = deriveCast<const StateCmdBase*>(baseCmd);
			dlg_assert(stateCmd->boundPipe());

			auto& job = patch_.jobs.emplace_back();
			job.data = std::make_unique<PatchJobData>();
			job.data->pipe.reset(stateCmd->boundPipe());
			job.data->compiler = compiled_.get();
			job.data->entryPoint = entryPoint_;
			job.data->stage = stage_;
			job.data->file = patch_.file;
			job.data->line = patch_.line;
			job.data->captureAddress = dev.commandHook->shaderCaptureAddress();
			// TODO: technically we have a race condition here (no data race tho).
			// when the user changes the invocation id in the ui before
			// this job finishes, it might get reset by the hook installed
			// by this job. Could fix it by checking for this condition
			// when retrieving the future and updating the hook.
			job.data->captureInput = globalInvocationID_;

			dlg_debug("Starting new patch job for {}:{}", patch_.file, patch_.line);
			job.result = std::async(std::launch::async,
				[&data = *job.data]{ return patchJob(data); });
		}
	}

	ImGui::EndChild();

	// variable view
	if(ImGui::BeginChild("Views")) {
		if(emu_) {
			emu_->drawControlTabs();
		} else {
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
	// TODO: decide depending on shader type
	using nytl::vec::cw::operators::operator*;

	auto wgs = workgroupSize();
	auto numWGs = numWorkgroups();
	auto numThreads = numWGs * wgs;
	bool changed = false;

	if(numThreads.x * numThreads.y * numThreads.z == 0u) {
		imGuiText("This dispatch call has no invocations!");
	} else {
		auto sliderFlags = 0u;
		globalInvocationID_.x = std::min(globalInvocationID_.x, numThreads.x - 1);
		globalInvocationID_.y = std::min(globalInvocationID_.y, numThreads.y - 1);
		globalInvocationID_.z = std::min(globalInvocationID_.z, numThreads.z - 1);
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
				int v = globalInvocationID_[i];
				changed |= ImGui::DragInt("", &v, 1.f, 0, numThreads[i] - 1, "%d", sliderFlags);
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
				auto before = globalInvocationID_[i] % wgs[i];
				int v = before;
				if(ImGui::DragInt("", &v, 1.f, 0, wgs[i] - 1, "%d", sliderFlags)) {
					globalInvocationID_[i] += v - before;
					changed = true;
				}
				ImGui::PopID();
			}

			ImGui::PopID();
		}
	}

	if(changed && patch_.current.pipe.vkHandle()) {
		updateHooks(*gui_->dev().commandHook);
	}
}

void ShaderDebugger::updateHooks(CommandHook& hook) {
	CommandHookUpdate hookUpdate;
	hookUpdate.invalidate = true;
	auto& ops = hookUpdate.newOps.emplace();
	ops.useCapturePipe = patch_.current.pipe.vkHandle();
	ops.capturePipeInput = Vec3u32(globalInvocationID_);
	hook.updateHook(std::move(hookUpdate));
}

void ShaderDebugPatch::updateJobs(Device& dev) {
	std::lock_guard lock(dev.mutex);

	for(auto it = jobs.begin(); it != jobs.end();) {
		auto& job = *it;
		auto res = job.result.wait_for(std::chrono::seconds(0));
		if(res != std::future_status::ready) {
			++it;
			continue;
		}

		if(job.data->state == PatchJobState::done) {
			if(current.pipe.vkHandle()) {
				keepAlive.push_back(std::move(current.pipe));
			}

			auto res = job.result.get();
			if(res.error.empty()) {
				dlg_debug("patch job has finished succesfully. {} captures",
					res.captures.size());
			} else {
				dlg_error("patch job error: {}", res.error);
			}

			current = std::move(res);
		}

		it = jobs.erase(it);
	}
}

void ShaderDebugPatch::reset(Device& dev) {
	for(auto it = jobs.begin(); it != jobs.end();) {
		auto& job = *it;

		auto state = [&]{
			std::lock_guard lock(dev.mutex);
			return job.data->state;
		}();

		if(state == PatchJobState::installing) {
			job.result.get();
			it = jobs.erase(it);
			continue;
		}

		{
			std::lock_guard lock(dev.mutex);
			job.data->state = PatchJobState::canceled;
		}
	}

	if(current.pipe.vkHandle()) {
		keepAlive.push_back(std::move(current.pipe));
	}

	current = {};
	file = u32(-1);
	line = u32(-1);
}

} // namespace vil
