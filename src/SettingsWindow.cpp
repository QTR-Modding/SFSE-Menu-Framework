#include "SettingsWindow.h"

#include "FrameworkSettings.h"
#include "Win32Platform.h"
#include "WindowManager.h"

#include <imgui.h>

#include <array>
#include <atomic>
#include <utility>

namespace SFSEMenuFramework::SettingsWindow
{
	namespace
	{
		// The separate settings-window presentation and toggle control directly
		// adapt SKSE Menu Framework 3 UI.cpp at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// Persistence and Starfield ownership updates remain SFSE-specific.
		constexpr char WINDOW_ID[] = "Settings##Window";

		bool isOpen{};
		bool focusRequested{};
		bool resetPlacement{};

		void ApplyRuntimeSettings()
		{
			auto* mainWindow = WindowManager::GetMainWindow();
			if (!mainWindow) {
				return;
			}

			mainWindow->PauseGame.store(
				FrameworkSettings::GetFreezeTimeOnMenu(),
				std::memory_order_release);
			mainWindow->BlurBackground.store(
				FrameworkSettings::GetBlurBackgroundOnMenu(),
				std::memory_order_release);
			static_cast<void>(Win32Platform::PostHostWindowCallback());
		}

		[[nodiscard]] bool RenderToggleMode(
			const char*                   a_label,
			FrameworkSettings::ToggleMode a_current,
			bool                          a_gamePad)
		{
			using ToggleMode = FrameworkSettings::ToggleMode;
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
				FrameworkSettings::SetToggleModeGamePad(mode) :
				FrameworkSettings::SetToggleMode(mode);
		}

		[[nodiscard]] bool RenderKeyboardBinding()
		{
			const auto current = FrameworkSettings::GetToggleKey();
			const auto currentName =
				FrameworkSettings::GetKeyboardBindingName(current);
			bool changed{};
			ImGui::TextUnformatted("Toggle key (keyboard)");
			if (ImGui::BeginCombo(
					"##KeyboardToggleKey",
					currentName.empty() ? "UNKNOWN" : currentName.data())) {
				for (const auto& binding : FrameworkSettings::GetKeyboardBindings()) {
					const bool selected = binding.Code == current;
					ImGui::PushID(static_cast<int>(binding.Code));
					if (ImGui::Selectable(binding.Name.data(), selected)) {
						changed = FrameworkSettings::SetToggleKey(binding.Code);
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
			const auto current = FrameworkSettings::GetToggleKeyGamePad();
			const auto currentName =
				FrameworkSettings::GetGamePadBindingName(current);
			bool changed{};
			ImGui::TextUnformatted("Toggle key (gamepad)");
			if (ImGui::BeginCombo(
					"##GamePadToggleKey",
					currentName.empty() ? "UNKNOWN" : currentName.data())) {
				for (const auto& binding : FrameworkSettings::GetGamePadBindings()) {
					const bool selected = binding.Code == current;
					ImGui::PushID(static_cast<int>(binding.Code));
					if (ImGui::Selectable(binding.Name.data(), selected)) {
						changed = FrameworkSettings::SetToggleKeyGamePad(binding.Code);
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

		[[nodiscard]] bool ToggleButton(const char* a_label, bool* a_value)
		{
			const auto position = ImGui::GetCursorScreenPos();
			auto* drawList = ImGui::GetWindowDrawList();
			const float height = ImGui::GetFrameHeight();
			const float width = height * 1.8F;
			const float radius = height * 0.5F;

			ImGui::PushID(a_label);
			ImGui::PushStyleColor(
				ImGuiCol_Header,
				ImVec4{ 0.0F, 0.0F, 0.0F, 0.0F });
			ImGui::PushStyleColor(
				ImGuiCol_HeaderHovered,
				ImVec4{ 0.0F, 0.0F, 0.0F, 0.0F });
			ImGui::PushStyleColor(
				ImGuiCol_HeaderActive,
				ImVec4{ 0.0F, 0.0F, 0.0F, 0.0F });
			const bool clicked =
				ImGui::Selectable("##toggle", false, 0, ImVec2{ width, height });
			ImGui::PopStyleColor(3);
			ImGui::PopID();
			if (clicked) {
				*a_value = !*a_value;
			}

			const float offset = *a_value ? 1.0F : 0.0F;
			const auto background = ImGui::GetColorU32(
				*a_value ? ImGuiCol_ButtonActive : ImGuiCol_Button);
			drawList->AddRectFilled(
				position,
				ImVec2{ position.x + width, position.y + height },
				background,
				height * 0.5F);
			drawList->AddCircleFilled(
				ImVec2{
					position.x + radius + offset * (width - radius * 2.0F),
					position.y + radius
				},
				radius - 1.5F,
				IM_COL32(255, 255, 255, 255));

			ImGui::SameLine();
			ImGui::TextUnformatted(a_label);
			return clicked;
		}

		void RenderFrameworkSettings()
		{
			static bool saveFailed{};
			bool changed{};
			changed =
				RenderToggleMode(
					"Toggle mode (keyboard)",
					FrameworkSettings::GetToggleMode(),
					false) ||
				changed;
			changed = RenderKeyboardBinding() || changed;

			ImGui::Separator();
			changed =
				RenderToggleMode(
					"Toggle mode (gamepad)",
					FrameworkSettings::GetToggleModeGamePad(),
					true) ||
				changed;
			changed = RenderGamePadBinding() || changed;

			ImGui::Separator();
			bool freeze = FrameworkSettings::GetFreezeTimeOnMenu();
			if (ToggleButton("Freeze time while menu is open", &freeze)) {
				FrameworkSettings::SetFreezeTimeOnMenu(freeze);
				changed = true;
			}
			bool blur = FrameworkSettings::GetBlurBackgroundOnMenu();
			if (ToggleButton("Blur background while menu is open", &blur)) {
				FrameworkSettings::SetBlurBackgroundOnMenu(blur);
				changed = true;
			}

			if (changed) {
				ApplyRuntimeSettings();
				saveFailed = !FrameworkSettings::Save();
			}

			ImGui::Spacing();
			if (ImGui::Button("Reset to defaults")) {
				FrameworkSettings::ResetDefaults();
				ApplyRuntimeSettings();
				saveFailed = !FrameworkSettings::Save();
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
	}

	void Open() noexcept
	{
		isOpen = true;
		focusRequested = true;
	}

	void Close() noexcept
	{
		isOpen = false;
		focusRequested = false;
	}

	void ResetPlacement() noexcept
	{
		resetPlacement = true;
	}

	void Render()
	{
		if (!isOpen) {
			return;
		}

		const auto* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(
			viewport->GetCenter(),
			resetPlacement ? ImGuiCond_Always : ImGuiCond_FirstUseEver,
			ImVec2{ 0.5F, 0.5F });
		ImGui::SetNextWindowSize(
			ImVec2{ viewport->Size.x * 0.4F, viewport->Size.y * 0.4F },
			resetPlacement ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
		resetPlacement = false;
		if (focusRequested) {
			ImGui::SetNextWindowFocus();
			focusRequested = false;
		}

		constexpr ImGuiWindowFlags windowFlags =
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_MenuBar |
			ImGuiWindowFlags_NoTitleBar;
		const bool drawContents = ImGui::Begin(WINDOW_ID, nullptr, windowFlags);
		if (drawContents && ImGui::BeginMenuBar()) {
			ImGui::TextUnformatted("Settings");
			const float barWidth = ImGui::GetWindowWidth();
			const float barHeight = ImGui::GetFrameHeight();
			const float closeButtonSize = barHeight;
			const float padding = ImGui::GetStyle().ItemSpacing.x;
			ImGui::SameLine(barWidth - closeButtonSize - padding);
			ImGui::PushStyleVar(
				ImGuiStyleVar_FramePadding,
				ImVec2{ 0.0F, 0.0F });
			if (ImGui::Button(
					"X",
					ImVec2{ closeButtonSize, closeButtonSize })) {
				isOpen = false;
			}
			ImGui::PopStyleVar();
			ImGui::EndMenuBar();
		}

		if (drawContents) {
			const float windowWidth = ImGui::GetContentRegionAvail().x;
			const float contentWidth = windowWidth * 0.8F;
			const float offset = (windowWidth - contentWidth) * 0.5F;
			if (offset > 0.0F) {
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
			}
			ImGui::BeginGroup();
			ImGui::PushItemWidth(contentWidth);
			RenderFrameworkSettings();
			ImGui::PopItemWidth();
			ImGui::EndGroup();
		}
		ImGui::End();
	}
}
