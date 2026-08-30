#include "McpWindow.h"

#include "FrameworkSettings.h"
#include "PanelRegistry.h"
#include "Win32Platform.h"
#include "WindowManager.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace
{
	constexpr char MCP_WINDOW_ID[] = "#MCPMainWindow";
	constexpr char MCP_TITLE[] = "Mod Control Panel";
	constexpr auto frameworkSettingsHandle =
		(std::numeric_limits<SFSEMenuFramework::Model::PanelHandle>::max)();
	SFSEMenuFramework::Model::PanelHandle selectedPanelHandle{ frameworkSettingsHandle };

	void ApplyRuntimeSettings()
	{
		auto* mainWindow = SFSEMenuFramework::WindowManager::GetMainWindow();
		if (!mainWindow) {
			return;
		}

		mainWindow->PauseGame.store(
			SFSEMenuFramework::FrameworkSettings::GetFreezeTimeOnMenu(),
			std::memory_order_release);
		mainWindow->BlurBackground.store(
			SFSEMenuFramework::FrameworkSettings::GetBlurBackgroundOnMenu(),
			std::memory_order_release);
		static_cast<void>(
			SFSEMenuFramework::Win32Platform::PostHostWindowCallback());
	}

	[[nodiscard]] bool RenderToggleMode(
		const char* a_label,
		SFSEMenuFramework::FrameworkSettings::ToggleMode a_current,
		bool a_gamePad)
	{
		using ToggleMode = SFSEMenuFramework::FrameworkSettings::ToggleMode;
		int selected = std::to_underlying(a_current);
		constexpr std::array names{
			"SINGLEPRESS",
			"HOLD",
			"DOUBLEPRESS",
			"OFF"
		};
		ImGui::TextUnformatted(a_label);
		if (!ImGui::Combo(
				(a_gamePad ? "##GamePadToggleMode" : "##KeyboardToggleMode"),
				&selected,
				names.data(),
				static_cast<int>(names.size()))) {
			return false;
		}

		const auto mode = static_cast<ToggleMode>(selected);
		return a_gamePad ?
			SFSEMenuFramework::FrameworkSettings::SetToggleModeGamePad(mode) :
			SFSEMenuFramework::FrameworkSettings::SetToggleMode(mode);
	}

	[[nodiscard]] bool RenderKeyboardBinding()
	{
		const auto current = SFSEMenuFramework::FrameworkSettings::GetToggleKey();
		const auto currentName =
			SFSEMenuFramework::FrameworkSettings::GetKeyboardBindingName(current);
		bool changed{};
		ImGui::TextUnformatted("Toggle key (keyboard)");
		if (ImGui::BeginCombo(
				"##KeyboardToggleKey",
				currentName.empty() ? "UNKNOWN" : currentName.data())) {
			for (const auto& binding :
				 SFSEMenuFramework::FrameworkSettings::GetKeyboardBindings()) {
				const bool selected = binding.Code == current;
				ImGui::PushID(static_cast<int>(binding.Code));
				if (ImGui::Selectable(binding.Name.data(), selected)) {
					changed = SFSEMenuFramework::FrameworkSettings::SetToggleKey(
						binding.Code);
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	[[nodiscard]] bool RenderGamePadBinding()
	{
		const auto current =
			SFSEMenuFramework::FrameworkSettings::GetToggleKeyGamePad();
		const auto currentName =
			SFSEMenuFramework::FrameworkSettings::GetGamePadBindingName(current);
		bool changed{};
		ImGui::TextUnformatted("Toggle key (gamepad)");
		if (ImGui::BeginCombo(
				"##GamePadToggleKey",
				currentName.empty() ? "UNKNOWN" : currentName.data())) {
			for (const auto& binding :
				 SFSEMenuFramework::FrameworkSettings::GetGamePadBindings()) {
				const bool selected = binding.Code == current;
				ImGui::PushID(static_cast<int>(binding.Code));
				if (ImGui::Selectable(binding.Name.data(), selected)) {
					changed =
						SFSEMenuFramework::FrameworkSettings::SetToggleKeyGamePad(
							binding.Code);
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	void RenderFrameworkSettings()
	{
		static bool saveFailed{};
		bool changed{};
		changed =
			RenderToggleMode(
				"Toggle mode (keyboard)",
				SFSEMenuFramework::FrameworkSettings::GetToggleMode(),
				false) ||
			changed;
		changed = RenderKeyboardBinding() || changed;

		ImGui::Spacing();
		changed =
			RenderToggleMode(
				"Toggle mode (gamepad)",
				SFSEMenuFramework::FrameworkSettings::GetToggleModeGamePad(),
				true) ||
			changed;
		changed = RenderGamePadBinding() || changed;

		ImGui::Spacing();
		bool freeze =
			SFSEMenuFramework::FrameworkSettings::GetFreezeTimeOnMenu();
		if (ImGui::Checkbox("Freeze time while menu is open", &freeze)) {
			SFSEMenuFramework::FrameworkSettings::SetFreezeTimeOnMenu(freeze);
			changed = true;
		}
		bool blur =
			SFSEMenuFramework::FrameworkSettings::GetBlurBackgroundOnMenu();
		if (ImGui::Checkbox("Blur background while menu is open", &blur)) {
			SFSEMenuFramework::FrameworkSettings::SetBlurBackgroundOnMenu(blur);
			changed = true;
		}

		if (changed) {
			ApplyRuntimeSettings();
			saveFailed = !SFSEMenuFramework::FrameworkSettings::Save();
		}

		ImGui::Spacing();
		if (ImGui::Button("Reset to defaults")) {
			SFSEMenuFramework::FrameworkSettings::ResetDefaults();
			ApplyRuntimeSettings();
			saveFailed = !SFSEMenuFramework::FrameworkSettings::Save();
		}
		ImGui::SameLine();
		if (ImGui::Button("Resume Game")) {
			static_cast<void>(
				SFSEMenuFramework::WindowManager::SetMainWindowOpen(false));
		}

		if (saveFailed) {
			ImGui::TextColored(
				ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F },
				"Could not save Data\\SFSE\\Plugins\\SFSEMenuFramework.ini");
		} else {
			ImGui::TextDisabled(
				"Settings file: Data\\SFSE\\Plugins\\SFSEMenuFramework.ini");
		}
	}

	void ApplyWindowPlacement(const ImGuiViewport* a_viewport)
	{
		ImGui::SetNextWindowPos(
			a_viewport->GetCenter(),
			ImGuiCond_FirstUseEver,
			ImVec2{ 0.5F, 0.5F });
		ImGui::SetNextWindowSize(
			ImVec2{ a_viewport->Size.x * 0.8F, a_viewport->Size.y * 0.8F },
			ImGuiCond_FirstUseEver);
	}

	void RenderRegisteredPanels(
		const SFSEMenuFramework::Model::RenderContext& a_context)
	{
		const auto snapshot = SFSEMenuFramework::PanelRegistry::GetSnapshot();
		static const SFSEMenuFramework::PanelRegistry::Snapshot emptyPanels;
		const auto& panels = snapshot ? *snapshot : emptyPanels;
		const auto isEnabled = [](const auto& a_panel) {
			return a_panel &&
			       a_panel->Enabled.load(std::memory_order_acquire);
		};

		auto selected = std::ranges::find_if(
			panels,
			[&](const auto& a_panel) {
				return isEnabled(a_panel) &&
				       a_panel->Handle == selectedPanelHandle;
			});
		if (selectedPanelHandle != frameworkSettingsHandle &&
			selected == panels.end()) {
			selectedPanelHandle = frameworkSettingsHandle;
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
			ImGui::SeparatorText("Framework");
			if (ImGui::Selectable(
					"Settings",
					selectedPanelHandle == frameworkSettingsHandle)) {
				selectedPanelHandle = frameworkSettingsHandle;
			}

			std::string_view previousSection;
			for (const auto& panel : panels) {
				if (!isEnabled(panel)) {
					continue;
				}
				if (panel->Section != previousSection) {
					ImGui::Spacing();
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
			if (selectedPanelHandle == frameworkSettingsHandle) {
				ImGui::SeparatorText("Settings");
				RenderFrameworkSettings();
			} else if (selected != panels.end()) {
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

	auto* windowInterface = WindowManager::AddWindow(&McpWindow::Render);
	if (!windowInterface || !WindowManager::SetMainWindow(windowInterface)) {
		return false;
	}

	windowInterface->BlockUserInput.store(true, std::memory_order_relaxed);
	windowInterface->PauseGame.store(
		FrameworkSettings::GetFreezeTimeOnMenu(),
		std::memory_order_relaxed);
	windowInterface->BlurBackground.store(
		FrameworkSettings::GetBlurBackgroundOnMenu(),
		std::memory_order_relaxed);
	return true;
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
