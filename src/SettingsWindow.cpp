#include "SettingsWindow.h"

#include "FontManager.h"
#include "FrameworkSettings.h"
#include "ThemeManager.h"
#include "Win32Platform.h"
#include "WindowManager.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
		bool fontSettingsRefreshRequested{ true };
		bool fontSettingsInvalid{};

		struct SettingsSnapshot final
		{
			std::uint32_t                   ToggleKey{};
			FrameworkSettings::ToggleMode   KeyboardMode{};
			std::uint32_t                   ToggleKeyGamePad{};
			FrameworkSettings::ToggleMode   GamePadMode{};
			bool                            FreezeTimeOnMenu{};
			bool                            BlurBackgroundOnMenu{};
			FrameworkSettings::MenuStyleName MenuStyle{};
			FrameworkSettings::FontSettings  Fonts{};
		};

		[[nodiscard]] SettingsSnapshot CaptureSettings() noexcept
		{
			return SettingsSnapshot{
				.ToggleKey = FrameworkSettings::GetToggleKey(),
				.KeyboardMode = FrameworkSettings::GetToggleMode(),
				.ToggleKeyGamePad = FrameworkSettings::GetToggleKeyGamePad(),
				.GamePadMode = FrameworkSettings::GetToggleModeGamePad(),
				.FreezeTimeOnMenu =
					FrameworkSettings::GetFreezeTimeOnMenu(),
				.BlurBackgroundOnMenu =
					FrameworkSettings::GetBlurBackgroundOnMenu(),
				.MenuStyle = FrameworkSettings::GetMenuStyle(),
				.Fonts = FrameworkSettings::GetFontSettings()
			};
		}

		void RestoreSettings(const SettingsSnapshot& a_snapshot) noexcept
		{
			static_cast<void>(
				FrameworkSettings::SetToggleKey(a_snapshot.ToggleKey));
			static_cast<void>(
				FrameworkSettings::SetToggleMode(a_snapshot.KeyboardMode));
			static_cast<void>(
				FrameworkSettings::SetToggleKeyGamePad(
					a_snapshot.ToggleKeyGamePad));
			static_cast<void>(
				FrameworkSettings::SetToggleModeGamePad(
					a_snapshot.GamePadMode));
			FrameworkSettings::SetFreezeTimeOnMenu(
				a_snapshot.FreezeTimeOnMenu);
			FrameworkSettings::SetBlurBackgroundOnMenu(
				a_snapshot.BlurBackgroundOnMenu);
			static_cast<void>(
				FrameworkSettings::SetMenuStyle(a_snapshot.MenuStyle.data()));
			static_cast<void>(
				FrameworkSettings::SetFontSettings(a_snapshot.Fonts));
		}

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

		[[nodiscard]] std::string_view FontNameView(
			const FrameworkSettings::FontFileName& a_name) noexcept
		{
			for (std::size_t index = 0; index < a_name.size(); ++index) {
				if (a_name[index] == '\0') {
					return { a_name.data(), index };
				}
			}
			return {};
		}

		[[nodiscard]] unsigned char ToUpperAscii(unsigned char a_value) noexcept
		{
			return a_value >= 'a' && a_value <= 'z' ?
				static_cast<unsigned char>(a_value - ('a' - 'A')) :
				a_value;
		}

		[[nodiscard]] bool EqualsIgnoreCaseAscii(
			std::string_view a_left,
			std::string_view a_right) noexcept
		{
			if (a_left.size() != a_right.size()) {
				return false;
			}
			for (std::size_t index = 0; index < a_left.size(); ++index) {
				if (ToUpperAscii(static_cast<unsigned char>(a_left[index])) !=
					ToUpperAscii(static_cast<unsigned char>(a_right[index]))) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool CopyFontName(
			std::string_view                    a_name,
			FrameworkSettings::FontFileName& a_result) noexcept
		{
			if (a_name.empty() || a_name.size() >= a_result.size()) {
				return false;
			}
			a_result.fill('\0');
			for (std::size_t index = 0; index < a_name.size(); ++index) {
				a_result[index] = a_name[index];
			}
			return true;
		}

		[[nodiscard]] bool NearlyEqual(float a_left, float a_right) noexcept
		{
			return std::fabs(a_left - a_right) <= 0.0001F;
		}

		[[nodiscard]] bool FontSettingsEqual(
			const FrameworkSettings::FontSettings& a_left,
			const FrameworkSettings::FontSettings& a_right) noexcept
		{
			return EqualsIgnoreCaseAscii(
					FontNameView(a_left.PrimaryFont),
					FontNameView(a_right.PrimaryFont)) &&
			       NearlyEqual(a_left.FontWeight, a_right.FontWeight) &&
			       NearlyEqual(a_left.FontSizeMedium, a_right.FontSizeMedium) &&
			       NearlyEqual(a_left.MinFontSize, a_right.MinFontSize) &&
			       NearlyEqual(a_left.MaxFontSize, a_right.MaxFontSize) &&
			       NearlyEqual(a_left.UIScale, a_right.UIScale);
		}

		[[nodiscard]] bool MatchesLiveSettings(
			const FrameworkSettings::FontSettings& a_settings) noexcept
		{
			return FontSettingsEqual(a_settings, FontManager::GetActiveSettings());
		}

		[[nodiscard]] bool QueueLiveFontSettings(
			const FrameworkSettings::FontSettings& a_settings) noexcept
		{
			return FrameworkSettings::ValidateFontSettings(a_settings) &&
			       FontManager::RequestAtlasRebuild(a_settings);
		}

		void RenderFontSettings(bool& a_saveFailed)
		{
			static FrameworkSettings::FontSettings pending{};
			if (fontSettingsRefreshRequested) {
				pending = FontManager::GetActiveSettings();
				fontSettingsRefreshRequested = false;
				fontSettingsInvalid = false;
			}

			ImGui::SeparatorText("Fonts");
			ImGui::TextUnformatted("Primary font");
			const auto pendingFontName = FontNameView(pending.PrimaryFont);

			const auto fonts = FontManager::GetFonts();
			const FontManager::FontEntry* pendingFont{};
			for (const auto& font : fonts) {
				if (EqualsIgnoreCaseAscii(pendingFontName, font.Name)) {
					pendingFont = &font;
					break;
				}
			}
			std::string missingFontPreview;
			const char* fontPreview = pending.PrimaryFont.data();
			if (pendingFontName.empty()) {
				fontPreview = "INVALID";
			} else if (!pendingFont) {
				missingFontPreview = "<missing: ";
				missingFontPreview.append(pendingFontName);
				missingFontPreview.push_back('>');
				fontPreview = missingFontPreview.c_str();
			}

			if (fonts.empty()) {
				ImGui::BeginDisabled();
				ImGui::Button("No fonts found##PrimaryFont");
				ImGui::EndDisabled();
			} else if (ImGui::BeginCombo("##PrimaryFont", fontPreview)) {
				for (std::size_t index = 0; index < fonts.size(); ++index) {
					const auto& name = fonts[index].Name;
					const bool selected =
						EqualsIgnoreCaseAscii(pendingFontName, name);
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::Selectable(name.c_str(), selected)) {
						if (!CopyFontName(name, pending.PrimaryFont)) {
							fontSettingsInvalid = true;
						} else {
							pendingFont = &fonts[index];
							if (pendingFont->WeightAxis) {
								pending.FontWeight = std::clamp(
									pending.FontWeight,
									pendingFont->WeightAxis->Minimum,
									pendingFont->WeightAxis->Maximum);
							}
							fontSettingsInvalid = !QueueLiveFontSettings(pending);
						}
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}

			pendingFont = nullptr;
			const auto selectedFontName = FontNameView(pending.PrimaryFont);
			for (const auto& font : fonts) {
				if (EqualsIgnoreCaseAscii(selectedFontName, font.Name)) {
					pendingFont = &font;
					break;
				}
			}

			if (pendingFont && pendingFont->WeightAxis) {
				const auto& axis = *pendingFont->WeightAxis;
				ImGui::Text(
					"Font weight (%.0f - %.0f)",
					axis.Minimum,
					axis.Maximum);
				if (ImGui::SliderFloat(
						"##FontWeight",
						&pending.FontWeight,
						axis.Minimum,
						axis.Maximum,
						"%.0f",
						ImGuiSliderFlags_AlwaysClamp)) {
					fontSettingsInvalid =
						!FrameworkSettings::ValidateFontSettings(pending);
				}
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					fontSettingsInvalid = !QueueLiveFontSettings(pending);
				}
				ImGui::TextDisabled(
					"Variable font; built-in default weight %.0f.",
					axis.Default);
			} else if (pendingFont) {
				ImGui::TextDisabled(
					"Font weight: fixed by this font file.");
			} else {
				ImGui::TextDisabled(
					"Font weight: unavailable until the font is found.");
			}

			ImGui::Text(
				"Font size (%.0f - %.0f)",
				pending.MinFontSize,
				pending.MaxFontSize);
			const bool fontSizeEdited = ImGui::InputFloat(
					"##FontSizeMedium",
					&pending.FontSizeMedium,
					1.0F,
					4.0F,
					"%.1f");
			if (fontSizeEdited) {
				fontSettingsInvalid =
					!FrameworkSettings::ValidateFontSettings(pending);
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				fontSettingsInvalid = !QueueLiveFontSettings(pending);
			}

			ImGui::TextUnformatted("UI scale");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Scales text, ImGui controls, and framework layout.");
			}
			int uiScalePercent = static_cast<int>(
				std::lround(pending.UIScale * 100.0F));
			if (ImGui::SliderInt(
					"##UIScale",
					&uiScalePercent,
					75,
					200,
					"%d%%")) {
				pending.UIScale = static_cast<float>(uiScalePercent) / 100.0F;
				fontSettingsInvalid =
					!FrameworkSettings::ValidateFontSettings(pending);
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				fontSettingsInvalid = !QueueLiveFontSettings(pending);
			}

			const auto activeName = FontManager::GetActiveFontName();
			const auto activeScalePercent = static_cast<int>(
				std::lround(FontManager::GetActiveUIScale() * 100.0F));
			if (FontManager::GetActiveWeightAxis()) {
				ImGui::TextDisabled(
					"Active: %.*s | weight %.0f | %.1f px | %d%% | %.1f raster px",
					static_cast<int>(activeName.size()),
					activeName.data(),
					FontManager::GetActiveFontWeight(),
					FontManager::GetActiveFontSize(),
					activeScalePercent,
					FontManager::GetActiveRasterSize());
			} else {
				ImGui::TextDisabled(
					"Active: %.*s | %.1f px | %d%% | %.1f raster px",
					static_cast<int>(activeName.size()),
					activeName.data(),
					FontManager::GetActiveFontSize(),
					activeScalePercent,
					FontManager::GetActiveRasterSize());
			}
			const auto fallbackReason = FontManager::GetFallbackReason();
			if (!fallbackReason.empty()) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.75F, 0.25F, 1.0F },
					"Fallback: %.*s",
					static_cast<int>(fallbackReason.size()),
					fallbackReason.data());
			}

			const bool pendingValid =
				FrameworkSettings::ValidateFontSettings(pending);
			const bool canSave = pendingValid && !fontSettingsInvalid &&
				!FontManager::HasPendingAtlasRebuild() &&
				MatchesLiveSettings(pending);
			if (!canSave) {
				ImGui::BeginDisabled();
			}
			if (ImGui::Button("Save")) {
				const auto previous = FrameworkSettings::GetFontSettings();
				if (!FrameworkSettings::SetFontSettings(pending)) {
					fontSettingsInvalid = true;
				} else if (!FrameworkSettings::Save()) {
					static_cast<void>(
						FrameworkSettings::SetFontSettings(previous));
					a_saveFailed = true;
				} else {
					pending = FrameworkSettings::GetFontSettings();
					fontSettingsInvalid = false;
					a_saveFailed = false;
				}
			}
			if (!canSave) {
				ImGui::EndDisabled();
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset font settings")) {
				pending = FrameworkSettings::GetDefaultFontSettings();
				fontSettingsInvalid = !QueueLiveFontSettings(pending);
			}

			if (fontSettingsInvalid || !pendingValid) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F },
					"Font weight must be 1 - 1000 and size must be within the displayed range; "
					"font size x UI scale must be at most 96 px.");
			}
			const auto configured = FrameworkSettings::GetFontSettings();
			const auto applyError = FontManager::GetLastApplyError();
			if (pendingValid && FontManager::HasPendingAtlasRebuild()) {
				ImGui::TextColored(
					ImVec4{ 0.35F, 0.8F, 1.0F, 1.0F },
					"Applying live font changes...");
			} else if (!applyError.empty()) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F },
					"%.*s",
					static_cast<int>(applyError.size()),
					applyError.data());
			} else if (pendingValid && !MatchesLiveSettings(pending)) {
				ImGui::TextDisabled("Finish editing to apply the live preview.");
			} else if (!FontSettingsEqual(pending, configured)) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.75F, 0.25F, 1.0F },
					"Live preview applied; changes are not saved.");
			} else {
				ImGui::TextDisabled(
					"Font, variable weight, and UI-scale changes apply live.");
			}
		}

		void RenderFrameworkSettings()
		{
			static bool saveFailed{};
			static bool themeLoadFailed{};
			const auto settingsBeforeRender = CaptureSettings();
			bool changed{};

			const auto themes = ThemeManager::GetThemes();
			const auto selectedTheme = ThemeManager::GetSelectedThemeIndex();
			const char* themePreview = selectedTheme < themes.size() ?
				themes[selectedTheme].Name.c_str() :
				"BUILT-IN DARK";
			ImGui::TextUnformatted("Menu style");
			if (themes.empty()) {
				ImGui::BeginDisabled();
				ImGui::Button("No themes found##MenuStyle");
				ImGui::EndDisabled();
			} else if (ImGui::BeginCombo("##MenuStyle", themePreview)) {
				for (std::size_t index = 0; index < themes.size(); ++index) {
					const bool selected = index == selectedTheme;
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::Selectable(themes[index].Name.c_str(), selected)) {
						if (ThemeManager::QueueTheme(index)) {
							changed = true;
							themeLoadFailed = false;
						} else {
							themeLoadFailed = true;
						}
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}

			RenderFontSettings(saveFailed);

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

			ImGui::Separator();
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

			if (changed) {
				ApplyRuntimeSettings();
				if (!FrameworkSettings::Save()) {
					RestoreSettings(settingsBeforeRender);
					themeLoadFailed = !ThemeManager::QueueConfiguredTheme();
					ApplyRuntimeSettings();
					saveFailed = true;
				} else {
					saveFailed = false;
				}
			}

			ImGui::Spacing();
			if (ImGui::Button("Reset to defaults")) {
				const auto settingsBeforeReset = CaptureSettings();
				FrameworkSettings::ResetDefaults();
				fontSettingsInvalid = !FontManager::RequestAtlasRebuild(
					FrameworkSettings::GetFontSettings());
				fontSettingsRefreshRequested = true;
				themeLoadFailed = !ThemeManager::QueueConfiguredTheme();
				ApplyRuntimeSettings();
				if (!FrameworkSettings::Save()) {
					RestoreSettings(settingsBeforeReset);
					fontSettingsInvalid = !FontManager::RequestAtlasRebuild(
						settingsBeforeReset.Fonts);
					fontSettingsRefreshRequested = true;
					themeLoadFailed = !ThemeManager::QueueConfiguredTheme();
					ApplyRuntimeSettings();
					saveFailed = true;
				} else {
					saveFailed = false;
				}
			}

			if (themeLoadFailed) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F },
					"Could not load the selected theme");
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
