#include "McpWindow.h"

#include "PanelRegistry.h"
#include "WindowManager.h"

#include <imgui.h>

#include <algorithm>
#include <string_view>

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
	SFSEMenuFramework::Model::PanelHandle selectedPanelHandle{};

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

	void RenderRegisteredPanels(
		const SFSEMenuFramework::Model::RenderContext& a_context)
	{
		const auto snapshot = SFSEMenuFramework::PanelRegistry::GetSnapshot();
		if (!snapshot || snapshot->empty()) {
			selectedPanelHandle = 0;
			ImGui::TextDisabled("No SFSE plugins have registered a panel.");
			return;
		}
		const auto& panels = *snapshot;
		const auto isEnabled = [](const auto& a_panel) {
			return a_panel &&
			       a_panel->Enabled.load(std::memory_order_acquire);
		};
		const auto firstEnabled = std::ranges::find_if(panels, isEnabled);
		if (firstEnabled == panels.end()) {
			selectedPanelHandle = 0;
			ImGui::TextDisabled("No SFSE plugin panels are currently available.");
			return;
		}

		auto selected = std::ranges::find_if(
			panels,
			[&](const auto& a_panel) {
				return isEnabled(a_panel) &&
				       a_panel->Handle == selectedPanelHandle;
			});
		if (selected == panels.end()) {
			selected = firstEnabled;
			selectedPanelHandle = (*selected)->Handle;
		}

		const auto available = ImGui::GetContentRegionAvail();
		const float navigationWidth =
			std::clamp(available.x * 0.25F, 180.0F, 320.0F);
		const auto childFlags =
			ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX;

		if (ImGui::BeginChild(
				"##MCPNavigation",
				ImVec2{ navigationWidth, 0.0F },
				childFlags)) {
			std::string_view previousSection;
			for (const auto& panel : panels) {
				if (!isEnabled(panel)) {
					continue;
				}
				if (panel->Section != previousSection) {
					if (!previousSection.empty()) {
						ImGui::Spacing();
					}
					ImGui::SeparatorText(panel->Section.c_str());
					previousSection = panel->Section;
				}

				ImGui::PushID(panel.get());
				if (ImGui::Selectable(
						panel->Title.c_str(),
						panel->Handle == selectedPanelHandle)) {
					selectedPanelHandle = panel->Handle;
					selected = std::ranges::find_if(
						panels,
						[&](const auto& a_candidate) {
							return isEnabled(a_candidate) &&
							       a_candidate->Handle == selectedPanelHandle;
						});
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();
		if (ImGui::BeginChild(
				"##MCPContent",
				ImVec2{ 0.0F, 0.0F },
				ImGuiChildFlags_Border)) {
			if (selected != panels.end()) {
				ImGui::SeparatorText((*selected)->Title.c_str());
				SFSEMenuFramework::PanelRegistry::Render(*selected, a_context);
			}
		}
		ImGui::EndChild();
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

void __stdcall SFSEMenuFramework::McpWindow::Render(
	const Model::RenderContext& a_context)
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

	if (drawContents) {
		RenderRegisteredPanels(a_context);
	}

	ImGui::End();
}
