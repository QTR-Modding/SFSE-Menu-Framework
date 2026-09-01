#pragma once

#include <imgui.h>

namespace SFSEMenuFramework::UI
{
	// These stateless presentation helpers are moved from the UI port of
	// SKSE Menu Framework 3 commit
	// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
	[[nodiscard]] inline bool RenderCloseButton()
	{
		const float size = ImGui::GetFrameHeight();
		ImGui::SameLine(
			ImGui::GetWindowWidth() - size - ImGui::GetStyle().ItemSpacing.x);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{});
		const bool close = ImGui::Button("X", ImVec2{ size, size });
		ImGui::PopStyleVar();
		return close;
	}

	inline void ApplyWindowPlacement(
		const ImGuiViewport& a_viewport,
		float                a_widthRatio,
		float                a_heightRatio,
		bool&                a_reset)
	{
		ImGui::SetNextWindowPos(
			a_viewport.GetCenter(),
			a_reset ? ImGuiCond_Always : ImGuiCond_FirstUseEver,
			ImVec2{ 0.5F, 0.5F });
		ImGui::SetNextWindowSize(
			ImVec2{
				a_viewport.Size.x * a_widthRatio,
				a_viewport.Size.y * a_heightRatio
			},
			a_reset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
		a_reset = false;
	}
}
