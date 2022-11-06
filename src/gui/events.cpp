#ifndef IMGUI_DEFINE_MATH_OPERATORS
	#define IMGUI_DEFINE_MATH_OPERATORS
#endif // IMGUI_DEFINE_MATH_OPERATORS

#include <gui/events.hpp>
#include <gui/gui.hpp>
#include <device.hpp>
#include <eventLog.hpp>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

namespace vil {

void EventViewer::draw(Gui& gui) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
	if(window->SkipItems) {
		return;
	}

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
	auto& io = g.IO;

    const ImRect frameBB(window->DC.CursorPos, window->DC.CursorPos + ImGui::GetContentRegionAvail());
    const ImRect innerBB(frameBB.Min + style.FramePadding, frameBB.Max - style.FramePadding);
    ImGui::ItemSize(frameBB, style.FramePadding.y);
    if(!ImGui::ItemAdd(frameBB, 0, &frameBB)) {
        return;
	}

	// ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
	if(ImGui::IsItemHovered() && io.MouseWheel != 0.f) {
		auto cx = offset_ + scale_ * (io.MousePos.x - ImGui::GetCursorScreenPos().x) / innerBB.GetWidth();

		auto x0 = cx - offset_;
		auto x1 = offset_ + scale_ - cx;

		auto sfac = std::pow(0.8, io.MouseWheel);
		x0 *= sfac;
		x1 *= sfac;

		scale_ = x0 + x1;
		offset_ = cx - x0;
	}

	// move
	if(ImGui::IsItemHovered() && io.MouseDown[1u]) {
		offset_ -= scale_ * io.MouseDelta.x / innerBB.GetWidth();
	}

    ImGui::RenderFrame(frameBB.Min, frameBB.Max,
		ImGui::GetColorU32(ImGuiCol_FrameBg), true, style.FrameRounding);

	// scale_ = std::min(scale_, 1.f);
	// offset_ = std::clamp(offset_, 0.f, 1.f - scale_);

	auto& dev = gui.dev();
	std::lock_guard lock(dev.eventLog->mutex);
	if (dev.eventLog->events.empty()) {
		return;
	}

	auto [width, height] = ImGui::GetContentRegionAvail();

	using Secd = std::chrono::duration<double, std::ratio<1, 1>>;
	using std::chrono::time_point_cast;
	auto event0 = time_point_cast<Secd>(dev.eventLog->events[0]->time);
	for(auto& event : dev.eventLog->events) {
		auto dur = time_point_cast<Secd>(event->time) - event0;
		auto rel = (dur.count() - offset_) / scale_;

		if (rel < 0.f || rel > 1.f) {
			continue;
		}

		const char* name {};
		if (event->type == EventType::queueSubmit) {
			name = "S";
		} else if (event->type == EventType::resourceCreated) {
			name = "+";
		} else if (event->type == EventType::resourceDestroyed) {
			name = "-";
		}

		const auto pos0 = innerBB.Min + ImVec2(rel * width, height);
		const auto pos1 = pos0 + ImVec2(20, 20);
		// window->DrawList->AddRectFilled(pos0, pos1, ImGui::GetColorU32(ImGuiCol_Button));

		auto textSize = ImGui::CalcTextSize(name);

		const auto x0 = innerBB.Min.x + std::max(rel, 0.0) * width;
		const auto x1 = x0 + 20.f;
		ImVec2 tp0 = ImVec2(x0, pos0.y);
		ImVec2 tp1 = ImVec2(x1, pos1.y);

		auto boxSize = (tp1 - tp0);
		if(textSize.x < boxSize.x) {
			auto textOffset = ImVec2(0.5f, 0.5f) * (boxSize - textSize);
			ImGui::RenderText(tp0 + textOffset, name);
		}
	}
}

} // namespace
