#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <queue.hpp>
#include <imguiutil.hpp>
#include <commands.hpp>
#include <imgui/imgui.h>
#include <vk/enumString.hpp>

namespace fuen {

CommandBufferGui::CommandBufferGui() = default;

CommandBufferGui::~CommandBufferGui() {
	auto& dev = gui_->dev();

	// TODO: probably (more efficiently) make sure we can destroy
	// our hooking resources!
	VK_CHECK(gui_->dev().dispatch.DeviceWaitIdle(dev.handle));

	if(hooked_.queryPool) {
		dev.dispatch.DestroyQueryPool(dev.handle, hooked_.queryPool, nullptr);
	}

	if(hooked_.commandPool) {
		dev.dispatch.DestroyCommandPool(dev.handle, hooked_.commandPool, nullptr);
	}
}

void CommandBufferGui::draw() {
	// auto& dev = gui_->dev();

	// Command list
	ImGui::Columns(2);
	ImGui::BeginChild("Command list", {400, 0});

#if 0
	// We can only display the content when the command buffer is in
	// executable state. The state of the command buffer is protected
	// by the device mutex (also command_ and the cb itself).
	if(!record_) {
		ImGui::Text("No command record selected");
		return;
	}

	// auto rec = cb_->lastRecordLocked();
	if(record_) { // might be nullptr when cb was never recorded
	// if(cb_->state() == CommandBuffer::State::executable) {
		// ImGui::PushID(dlg::format("{}:{}", cb_, cb_->resetCount).c_str());
		ImGui::PushID(dlg::format("{}", cb_).c_str());

		if(rec->recordID != recordID_) {
			// try to find a new command matching the old ones description
			command_ = CommandDesc::find(rec->commands, desc_);

			/*
			if(!desc_.empty()) {
				std::string indent = "  ";
				for(auto& lvl : desc_) {
					dlg_trace("{}{} {}", indent, lvl.command, lvl.id);
					for(auto& arg : lvl.arguments) {
						dlg_trace("{}    {}", indent, arg);
					}
					indent += " ";
				}

				dlg_trace("-> new command: {}", command_);
			}
			*/

			hooked_.needsUpdate = true;
		}

		// TODO: add selector ui to filter out various commands/don't
		// show sections etc. Should probably pass a struct DisplayDesc
		// to displayCommands instead of various parameters
		auto flags = Command::TypeFlags(nytl::invertFlags, Command::Type::end);
		auto* nsel = displayCommands(rec->commands, command_, flags);
		if(nsel) {
			recordID_ = rec->recordID;
			desc_ = CommandDesc::get(*rec->commands, *nsel);
			command_ = nsel;
			hooked_.needsUpdate = true;
		}

		ImGui::PopID();
	} else {
		ImGui::Text("[Not in exeuctable state]");
		command_ = nullptr;
	}
#endif

	if(group_->lastRecord.get() != record_) {
		ImGui::PushID(dlg::format("{}", group_).c_str());

		record_ = group_->lastRecord.get();
		command_ = CommandDesc::find(record_->commands, desc_);

		// TODO: add selector ui to filter out various commands/don't
		// show sections etc. Should probably pass a struct DisplayDesc
		// to displayCommands instead of various parameters
		auto flags = Command::TypeFlags(nytl::invertFlags, Command::Type::end);
		auto* nsel = displayCommands(record_->commands, command_, flags);
		if(nsel) {
			desc_ = CommandDesc::get(*record_->commands, *nsel);
			command_ = nsel;
			// hooked_.needsUpdate = true;
		}

		ImGui::PopID();
	}

	ImGui::EndChild();
	ImGui::NextColumn();

	// command info
	ImGui::BeginChild("Command Info", {600, 0});
	if(command_) {
		// Inspector
		command_->displayInspector(*gui_);

		/*
		// Show own general gui
		ImGui::Checkbox("Query Time", &hooked_.query);
		if(hooked_.query) {
			if(!hooked_.queryPool) {
				VkQueryPoolCreateInfo qci {};
				qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
				qci.queryCount = 10u;
				qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
				VK_CHECK(dev.dispatch.CreateQueryPool(dev.handle, &qci, nullptr, &hooked_.queryPool));
				nameHandle(dev, hooked_.queryPool, "CbGui:queryPool");
			}

			if(!hooked_.commandPool) {
				VkCommandPoolCreateInfo cpci {};
				cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
				cpci.queueFamilyIndex = cb_->pool().queueFamily;
				cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
				VK_CHECK(dev.dispatch.CreateCommandPool(dev.handle, &cpci, nullptr, &hooked_.commandPool));
				nameHandle(dev, hooked_.commandPool, "CbGui:commandPool");

				VkCommandBufferAllocateInfo cbai {};
				cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
				cbai.commandBufferCount = 1u;
				cbai.commandPool = hooked_.commandPool;
				cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				VK_CHECK(dev.dispatch.AllocateCommandBuffers(dev.handle, &cbai, &hooked_.cb));
				nameHandle(dev, hooked_.cb, "CbGui:cb");

				// command buffer is a dispatchable object
				dev.setDeviceLoaderData(dev.handle, hooked_.cb);
			}

			const auto stages = {
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
				VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
				VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
				VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT,
				VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT,
				VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
				VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_HOST_BIT,
				VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				// VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
				// VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT,
				// VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
				// VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
				// VK_PIPELINE_STAGE_SHADING_RATE_IMAGE_BIT_NV,
				// VK_PIPELINE_STAGE_TASK_SHADER_BIT_NV,
				// VK_PIPELINE_STAGE_MESH_SHADER_BIT_NV,
				// VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT,
				// VK_PIPELINE_STAGE_COMMAND_PREPROCESS_BIT_NV,
				// VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_NV,
				// VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_NV,
				// VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
			};

			auto stageCombo = [&stages](auto name, auto& choice) {
				auto choiceName = vk::name(choice);
				auto newChoice = choice;
				if(ImGui::BeginCombo(name, choiceName)) {
					for(auto& stage : stages) {
						if(ImGui::Selectable(vk::name(stage))) {
							newChoice = stage;
						}
					}

					ImGui::EndCombo();
				}

				auto ret = newChoice != choice;
				choice = newChoice;
				return ret;
			};

			hooked_.needsUpdate |= stageCombo("Start", hooked_.queryStart);
			hooked_.needsUpdate |= stageCombo("End", hooked_.queryEnd);

			cb_->hook = [this](CommandBuffer& cb) { return cbHook(cb); };

			// get results
			// We could wait for the results here if we track that a command
			// buffer was submitted. But probably not worth it, we only
			// display it if available (that's why we waited for the submissions
			// to complete in Gui).
			u64 data[20];
			auto res = dev.dispatch.GetQueryPoolResults(dev.handle, hooked_.queryPool, 0, 3,
				sizeof(data), data, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);

			// check if query is available
			if(res == VK_SUCCESS && (data[1] && data[3] && data[5])) {
				u64 before = data[0];
				u64 afterStart = data[2];
				u64 afterEnd = data[4];

				// TODO: can't say i'm sure about this calculation
				auto start = std::max(afterStart, before);
				auto diff = afterEnd - start;

				auto displayDiff = diff * dev.props.limits.timestampPeriod;
				auto timeNames = {"ns", "mus", "ms", "s"};

				auto it = timeNames.begin();
				while(displayDiff > 1000.f && (it + 1) != timeNames.end()) {
					++it;
					displayDiff /= 1000.f;

				}

				imGuiText("Time: {} {}", displayDiff, *it);
			}
		}
		*/
	}

	ImGui::EndChild();
	ImGui::Columns();
}

VkCommandBuffer CommandBufferGui::cbHook(CommandBuffer& cb) {
	return cb.handle();

	/*
	dlg_assert(&cb == cb_);

	// TODO: already try to find the new corresponding command?
	auto rec = cb.lastRecordLocked();
	if(rec->recordID != recordID_) {
		return cb.handle();
	}

	auto& dev = gui_->dev();
	if(hooked_.needsUpdate) {
		// TODO: this might not work for simulataneous-use command buffers!
		// we can't be sure that the command buffer isn't in use anymore.
		// Would need to dynamically allocate a new command buffer here
		// if that is the case (or re-use and old, now unused one from
		// the pool (would likely need our own internal cb pool))
		VkCommandBufferBeginInfo cbbi {};
		cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		VK_CHECK(dev.dispatch.BeginCommandBuffer(hooked_.cb, &cbbi));

		dev.dispatch.CmdResetQueryPool(hooked_.cb, hooked_.queryPool, 0, 10);

		hookRecord(rec->commands);

		VK_CHECK(dev.dispatch.EndCommandBuffer(hooked_.cb));
	}

	return hooked_.cb;
	*/
}

// void CommandBufferGui::hookRecord(const std::vector<CommandPtr>& commands) {
// void CommandBufferGui::hookRecord(const CommandVector<CommandPtr>& commands) {
void CommandBufferGui::hookRecord(const Command* cmd) {
	auto& dev = gui_->dev();

	while(cmd) {
		if(cmd == command_ && hooked_.query) {
			// dev.dispatch.CmdWriteTimestamp(hooked_.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, hooked_.queryPool, 0);
			dev.dispatch.CmdWriteTimestamp(hooked_.cb, hooked_.queryEnd, hooked_.queryPool, 0);
		}

		cmd->record(dev, hooked_.cb);

		// TODO: dynamic cast kinda ugly.
		// We should probably have a general visitor mechanism instead
		if(auto sectionCmd = dynamic_cast<const SectionCommand*>(cmd); sectionCmd) {
			hookRecord(sectionCmd->children);
		}

		if(cmd == command_ && hooked_.query) {
			dev.dispatch.CmdWriteTimestamp(hooked_.cb, hooked_.queryStart, hooked_.queryPool, 1);
			dev.dispatch.CmdWriteTimestamp(hooked_.cb, hooked_.queryEnd, hooked_.queryPool, 2);
			// dev.dispatch.CmdWriteTimestamp(hooked_.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, hooked_.queryPool, 3);
		}

		cmd = cmd->next;
	}
}

void CommandBufferGui::select(CommandBufferGroup& group) {
	group_ = &group;
	command_ = {};
	record_ = group.lastRecord.get();

	if(record_ && !desc_.empty()) {
		command_ = CommandDesc::find(record_->commands, desc_);
	}
}

/*
void CommandBufferGui::select(CommandBuffer& cb) {
	if(cb_ && cb_->hook) {
		cb_->hook = {};
	}

	// TODO: we might be able to re-use it
	if(hooked_.commandPool) {
		gui_->dev().dispatch.DestroyCommandPool(gui_->dev().handle, hooked_.commandPool, nullptr);
		hooked_.commandPool = {};
		hooked_.cb = {};
	}

	cb_ = &cb;
	command_ = {};

	// auto* rec = cb_->lastRecordLocked();
	auto* rec = cb_->lastRecordLocked();
	if(!desc_.empty()) {
		command_ = CommandDesc::find(rec->commands, desc_);
	}

	group_ = rec->group;
	// recordID_ = rec->recordID;

	hooked_.query = false;
	hooked_.needsUpdate = true;
	hooked_.queryStart = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	hooked_.queryEnd = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
}
*/

void CommandBufferGui::destroyed(const Handle& handle) {
	(void) handle;
	/*
	if(&handle == cb_) {
		cb_ = nullptr;
		command_ = nullptr;
		recordID_ = {};

		// TODO: hacky
		if(group_ && group_->lastRecord->cb) {
			select(*group_->lastRecord->cb);
		}
	}
	*/
}

} // namespace fuen
