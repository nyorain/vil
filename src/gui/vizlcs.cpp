#ifndef IMGUI_DEFINE_MATH_OPERATORS
	#define IMGUI_DEFINE_MATH_OPERATORS
#endif // IMGUI_DEFINE_MATH_OPERATORS

#include <gui/vizlcs.hpp>
#include <imgui/imgui_internal.h>
#include <random>
#include <iomanip>

namespace vil {

// viz
void drawTexCentered(ImDrawList* dl, ImVec2 pos, u32 color, const char* text) {
	auto textSize = ImGui::CalcTextSize(text);
	dl->AddText(pos - ImVec2(0.5f, 0.5f) * textSize, color, text);
}

void drawField(ImDrawList* dl, ImVec2 pos, ImVec2 size, u32 color) {
	dl->AddRectFilled(pos, pos + size, color);
}

void drawWeight(ImDrawList* dl, ImVec2 pos, ImVec2 size, double weight) {
	auto tp = pos + ImVec2(0.5f, 0.5f) * size;
	auto textStr = dlg::format("{}{}", std::setprecision(3), weight);
	drawTexCentered(dl, tp, 0xFF000000, textStr.c_str());
}

void VizLCS::draw() {
	auto width = algo_.width();
	auto height = algo_.height();

	if(ImGui::Button("Step")) {
		algo_.step();
	}

	auto* dl = ImGui::GetWindowDrawList();

	ImVec2 size{40.f, 40.f};
	ImVec2 pad{8.f, 8.f};

	auto spos = ImGui::GetCursorScreenPos();

	// first, draw all fields, basic
	for(auto y = 0u; y < height; ++y) {
		for(auto x = 0u; x < width; ++x) {
			ImVec2 pos = spos + ImVec2(x, y) * (pad + size);
			drawField(dl, pos, size, 0xFFFFFFFFu);
		}
	}

	// now, draw all candidates
	for(auto& cand : algo_.candidates()) {
		ImVec2 pos = spos + ImVec2(cand.i, cand.j) * (pad + size);
		drawField(dl, pos, size, 0xFF6666FFu);

		auto score = cand.score;
		drawWeight(dl, pos + ImVec2(0, 15), size, score);
	}

	// now, draw best candidate
	if(!algo_.empty()) {
		auto cand = algo_.peekCandidate();
		ImVec2 pos = spos + ImVec2(cand.i, cand.j) * (pad + size);
		drawField(dl, pos, size, 0xFFFF66FFu);
	}


	// now, draw best paths on all discovered fields
	for(auto y = 0u; y < height; ++y) {
		for(auto x = 0u; x < width; ++x) {
			if(algo_.matchData(x, y).eval == -1.f) {
				continue;
			}

			ImVec2 pos = spos + ImVec2(x, y) * (pad + size);
			drawWeight(dl, pos, size, algo_.matchData(x, y).best);
		}
	}

	// if done: viz results
	if(algo_.empty()) {
		auto res = algo_.run(); // collect

		for(auto m : res.matches) {
			ImVec2 pos = spos + ImVec2(m.i, m.j) * (pad + size);
			drawField(dl, pos, size, 0xFF66FF66u);
			drawWeight(dl, pos, size, m.matchVal);
		}
	}
}

constexpr auto width = 12u;
constexpr auto height = 7u;

VizLCS::VizLCS() : algo_(width, height, alloc_,
		[this](u32 x, u32 y){ return weights_[width * y + x]; }) {
	weights_ = std::make_unique<float[]>(width * height);

	// semi random weights
	std::random_device rd;
	std::mt19937 e2(rd());
	std::uniform_real_distribution<> dist(0, 1);
	for(auto y = 0u; y < height; ++y) {
		for(auto x = 0u; x < width; ++x) {
			auto weight = dist(e2);

			/*
			auto weight = 0.f;
			float w = std::max(dist(e2), 0.0);
			w = 1 - w * w * w;
			if(w > 0.8) { //0.1 * std::abs(int(x) - int(y))) {
				weight = std::max(std::min(w, 1.f), 0.f);
			}
			*/

			// TODO: with this, an expected value on the boundary fails.
			// figure out the correct solution
			// if(x == y) {
			// 	weight = 1.f;
			// }

			weights_[y * width + x] = weight;
		}
	}
}

} // namespace vil
