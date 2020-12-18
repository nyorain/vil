#include <gui/cb.hpp>
#include <gui/gui.hpp>
#include <imguiutil.hpp>
#include <commands.hpp>
#include <imgui/imgui.h>

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
	auto& dev = gui_->dev();

	// Command list
	ImGui::Columns(2);
	ImGui::BeginChild("Command list", {400, 0});

	// We can only display the content when the command buffer is in
	// executable state. The state of the command buffer is protected
	// by the device mutex (also command_ and the cb itself).
	if(!cb_) {
		ImGui::Text("No command buffer selected");
		return;
	}

	if(cb_->state == CommandBuffer::State::executable) {
		ImGui::PushID(dlg::format("{}:{}", cb_, cb_->resetCount).c_str());

		if(cb_->resetCount != resetCount_) {
			// try to find a new command matching the old ones description
			command_ = CommandDescription::find(*cb_, desc_);
			hooked_.needsUpdate = true;
		}

		// TODO: add selector ui to filter out various commands/don't
		// show sections etc. Should probably pass a struct DisplayDesc
		// to displayCommands instead of various parameters
		auto flags = Command::TypeFlags(nytl::invertFlags, Command::Type::end);
		auto* nsel = displayCommands(cb_->commands, command_, flags);
		if(nsel) {
			resetCount_ = cb_->resetCount;
			desc_ = CommandDescription::get(*cb_, *nsel);
			command_ = nsel;
			hooked_.needsUpdate = true;
		}

		ImGui::PopID();
	} else {
		ImGui::Text("[Not in exeuctable state]");
		command_ = nullptr;
	}

	ImGui::EndChild();
	ImGui::NextColumn();

	// command info
	ImGui::BeginChild("Command Info", {600, 0});
	if(command_) {
		// Inspector
		command_->displayInspector(*gui_);

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
				cpci.queueFamilyIndex = cb_->pool->queueFamily;
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
	}

	ImGui::EndChild();
	ImGui::Columns();
}

VkCommandBuffer CommandBufferGui::cbHook(CommandBuffer& cb) {
	dlg_assert(&cb == cb_);

	if(cb.resetCount != resetCount_) {
		return cb.handle;
	}

	auto& dev = gui_->dev();
	if(hooked_.needsUpdate) {
		VkCommandBufferBeginInfo cbbi {};
		cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		VK_CHECK(dev.dispatch.BeginCommandBuffer(hooked_.cb, &cbbi));
		dev.dispatch.CmdResetQueryPool(hooked_.cb, hooked_.queryPool, 0, 10);

		hookRecord(cb.commands);

		VK_CHECK(dev.dispatch.EndCommandBuffer(hooked_.cb));
	}

	return hooked_.cb;
}

void CommandBufferGui::hookRecord(span<const CommandPtr> commands) {
	auto& dev = gui_->dev();
	for(auto& cmd : commands) {
		if(cmd.get() == command_ && hooked_.query) {
			// dev.dispatch.CmdWriteTimestamp(hooked_.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, hooked_.queryPool, 0);
			dev.dispatch.CmdWriteTimestamp(hooked_.cb, hooked_.queryEnd, hooked_.queryPool, 0);
		}

		cmd->record(dev, hooked_.cb);

		// TODO: dynamic cast kinda ugly.
		// We should probably have a general visitor mechanism instead
		if(auto sectionCmd = dynamic_cast<const SectionCommand*>(cmd.get()); sectionCmd) {
			hookRecord(sectionCmd->children);
		}

		if(cmd.get() == command_ && hooked_.query) {
			dev.dispatch.CmdWriteTimestamp(hooked_.cb, hooked_.queryStart, hooked_.queryPool, 1);
			dev.dispatch.CmdWriteTimestamp(hooked_.cb, hooked_.queryEnd, hooked_.queryPool, 2);
			// dev.dispatch.CmdWriteTimestamp(hooked_.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, hooked_.queryPool, 3);
		}
	}
}

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
	if(!desc_.empty()) {
		command_ = CommandDescription::find(cb, desc_);
	}

	resetCount_ = cb_->resetCount;

	hooked_.query = false;
	hooked_.needsUpdate = true;
	hooked_.queryStart = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	hooked_.queryEnd = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
}

void CommandBufferGui::destroyed(const Handle& handle) {
	if(&handle == cb_) {
		cb_ = nullptr;
		command_ = nullptr;
		resetCount_ = {};
	}
}

} // namespace fuen
