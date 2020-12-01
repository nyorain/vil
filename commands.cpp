#include "commands.hpp"
#include "cb.hpp"

namespace fuen {

const Command* ExecuteCommandsCmd::display(const Command* selected, TypeFlags typeFlags) const {
	auto flags = ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_OpenOnArrow;
	if(this == selected) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	const Command* ret = nullptr;
	if(ImGui::TreeNodeEx(this, flags, "CmdExecuteCommands")) {
		if(ImGui::IsItemClicked()) {
			ret = this;
		}

		// The most common use case: one single command buffer is executed.
		// In that case we show a simpler UI
		if(secondaries.size() == 1) {
			auto* cmd = secondaries[0];
			auto reti = displayCommands(cmd->commands, selected, typeFlags);
			if(reti) {
				dlg_assert(!ret);
				ret = reti;
			}
		} else {
			for(auto* cmd : secondaries) {
				auto cname = name(*cmd);
				auto treeID = dlg::format("{}:{}", this, cname);
				if(ImGui::TreeNode(treeID.c_str(), "%s", cname.c_str())) {
					auto reti = displayCommands(cmd->commands, selected, typeFlags);
					if(reti) {
						dlg_assert(!ret);
						ret = reti;
					}

					ImGui::TreePop();
				}
			}
		}

		ImGui::TreePop();
	}

	return ret;
}

} // namespace fuen
