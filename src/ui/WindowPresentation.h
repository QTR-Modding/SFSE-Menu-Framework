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

}
