#include "McpWindow.h"

#include "WindowManager.h"

#include <imgui.h>

namespace
{
	constexpr char MCP_WINDOW_ID[] = "#MCPMainWindow";
	constexpr char MCP_TITLE[] = "Mod Control Panel";

	struct WindowPlacement final
	{
		bool   HasState{ false };
		ImVec2 Position{};
		ImVec2 Size{};
	};

	WindowPlacement mainWindowPlacement;

	void ApplyWindowPlacement(const ImGuiViewport* a_viewport)
	{
		if (mainWindowPlacement.HasState) {
			ImGui::SetNextWindowPos(mainWindowPlacement.Position, ImGuiCond_Appearing);
			ImGui::SetNextWindowSize(mainWindowPlacement.Size, ImGuiCond_Appearing);
			return;
		}

		ImGui::SetNextWindowPos(a_viewport->GetCenter(), ImGuiCond_Appearing, ImVec2{ 0.5F, 0.5F });
		ImGui::SetNextWindowSize(
			ImVec2{ a_viewport->Size.x * 0.8F, a_viewport->Size.y * 0.8F },
			ImGuiCond_Appearing);
	}

	void SaveWindowPlacement()
	{
		mainWindowPlacement.Position = ImGui::GetWindowPos();
		mainWindowPlacement.Size = ImGui::GetWindowSize();
		mainWindowPlacement.HasState = true;
	}
}

bool SFSEMenuFramework::McpWindow::Install()
{
	if (WindowManager::GetMainWindow()) {
		return false;
	}

	auto* interface = WindowManager::AddWindow(&McpWindow::Render);
	if (!interface || !WindowManager::SetMainWindow(interface)) {
		return false;
	}

	interface->BlockUserInput.store(true, std::memory_order_relaxed);
	return WindowManager::SetMainWindowOpen(true);
}

void __stdcall SFSEMenuFramework::McpWindow::Render()
{
	const auto* viewport = ImGui::GetMainViewport();
	ApplyWindowPlacement(viewport);

	constexpr ImGuiWindowFlags windowFlags =
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_MenuBar |
		ImGuiWindowFlags_NoTitleBar;

	const bool drawContents = ImGui::Begin(MCP_WINDOW_ID, nullptr, windowFlags);
	SaveWindowPlacement();

	if (drawContents && ImGui::BeginMenuBar()) {
		const float barWidth = ImGui::GetWindowWidth();
		const float barHeight = ImGui::GetFrameHeight();
		const float textWidth = ImGui::CalcTextSize(MCP_TITLE).x;
		const float closeButtonSize = barHeight;
		const float padding = ImGui::GetStyle().ItemSpacing.x;
		const float availableWidth = barWidth - closeButtonSize - padding;
		const float titlePosition = availableWidth * 0.5F - textWidth * 0.5F;

		ImGui::SameLine(titlePosition);
		ImGui::TextUnformatted(MCP_TITLE);

		const float closeButtonPosition = barWidth - closeButtonSize - padding;
		ImGui::SameLine(closeButtonPosition);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{ 0.0F, 0.0F });

		if (ImGui::Button("X", ImVec2{ closeButtonSize, closeButtonSize })) {
			static_cast<void>(WindowManager::SetMainWindowOpen(false));
		}

		ImGui::PopStyleVar();
		ImGui::EndMenuBar();
	}

	ImGui::End();
}
