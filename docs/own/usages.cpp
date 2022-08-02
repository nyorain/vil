// too broken atm, was just a prototype.
// in ResourceGui::drawHandleDesc(Draw& draw, Handle& handle)
// hard to implement for dynamic descriptor sets and records
constexpr auto enableUsages = false;

// too broken atm
auto showUsages = [&](const UsedHandle& uh, const char* name) {
	if(ImGui::TreeNode(&handle, "%s", name)) {
		for(auto& cmd : uh.commands) {
			auto n = cmd->toString();

			ImGui::PushID(&cmd);
			if(ImGui::Button(n.c_str())) {
				gui_->activateTab(Gui::Tab::commandBuffer);
				auto recPtr = IntrusivePtr<CommandRecord>(uh.record);
				gui_->cbGui().select(std::move(recPtr), cmd);
			}

			ImGui::PopID();
		}

		ImGui::TreePop();
	}
};

auto showDirectUsages = [&](const DeviceHandle& dh) {
	auto it = dh.refRecords;
	while(it) {
		auto name = it->record->cbName;
		if(!name) {
			name = "unnamed";
		}

		ImGui::PushID(it->record);
		showUsages(*it, name);
		ImGui::PopID();

		it = it->next;
	}
};

auto showDsUsages = [&](const DescriptorSet& ds) {
	// TODO: ugly af.
	// This should work even if there isn't any swapchain!
	if(!gui_->dev().swapchain) {
		return;
	}

	auto& subs = gui_->dev().swapchain->frameSubmissions.front();
	for(auto& batch : subs.batches) {
		for(auto& rec : batch.submissions) {

			// TODO: why isn't this const?? :(
			auto it = rec->handles.find(&const_cast<DescriptorSet&>(ds));
			if(it == rec->handles.end()) {
				continue;
			}

			auto name = dlg::format("{} via {}",
				it->second->record->cbName ? it->second->record->cbName : "<unnamed record>",
				vil::name(ds));

			ImGui::PushID(rec.get());
			ImGui::PushID(&ds);

			showUsages(*it->second, name.c_str());

			ImGui::PopID();
			ImGui::PopID();
		}
	}
};

auto visitor = TemplateResourceVisitor([&](auto& res) {
	if(editName_) {
		imGuiTextInput("", handle.name);
		if(ImGui::IsItemDeactivated()) {
			editName_ = false;
		}

		// TODO: forward new debug name to further layers?
		// not sure if that's expected behavior
	} else {
		imGuiText("{}", name(res));
		if(ImGui::IsItemClicked()) {
			editName_ = true;
		}
	}

	ImGui::Spacing();

	this->drawDesc(draw, res);

	if(!enableUsages) {
		return;
	}

	using H = std::remove_reference_t<decltype(res)>;
	if constexpr(std::is_base_of_v<DeviceHandle, H>) {
		if(ImGui::TreeNode("Command Usages")) {
			auto& dh = static_cast<DeviceHandle&>(res);
			showDirectUsages(dh);

			imGuiText("Via Descriptor Set:");

			// uses via descriptor sets
			// TODO: speed up! e.g. initial (async) search and
			//   then notifications if used in DescriptorUpdate
			for(auto& [h, pool] : gui_->dev().dsPools.inner) {
				auto lock = std::scoped_lock(pool->mutex);
				for(auto it = pool->usedEntries; it; it = it->next) {
					dlg_assert(it->set);
					auto lock2 = it->set->lock();

					// TODO: instead of showing the commands
					// where it's *bound*, show the commands
					// where it's *used*. Should be somewhat
					// easy to determine, just start the binding command
					// and then go forward?
					// maybe show both here? show the binding
					// command first and then expand a TreeNode
					// to see all following usages?
					// OH, XXX even better, in the inspector of
					// CmdBindX show *all* future commands affected
					// by it.
					// For CmdBindDescriptorSets we could even
					// split this up per-descriptorSet (or even
					// per descriptor?).
					if(hasBound(*it->set, dh)) {
						showDsUsages(*it->set);
					}
				}
			}

			ImGui::TreePop();
		}
	}
});
