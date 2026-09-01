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

		[[nodiscard]] bool SaveOrRestore(
			const FrameworkSettings::SettingsSnapshot& a_previous,
			bool                                       a_rebuildFonts,
			bool&                                      a_themeLoadFailed)
		{
			if (FrameworkSettings::Save()) {
				return true;
			}
			FrameworkSettings::RestoreSnapshot(a_previous);
			if (a_rebuildFonts) {
				fontSettingsInvalid = !FontManager::RequestAtlasRebuild(a_previous.Fonts);
				fontSettingsRefreshRequested = true;
			}
			a_themeLoadFailed = !ThemeManager::QueueConfiguredTheme();
			ApplyRuntimeSettings();
			return false;
		}

		template <class Setter>
		[[nodiscard]] bool RenderToggleMode(
			const char*                   a_label,
			const char*                   a_id,
			FrameworkSettings::ToggleMode a_current,
			Setter                        a_set)
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
					a_id,
					&selected,
					names.data(),
					static_cast<int>(names.size()))) {
				return false;
			}

			return a_set(static_cast<ToggleMode>(selected));
		}

		template <class Setter>
		[[nodiscard]] bool RenderBinding(
			const char*                                a_label,
			const char*                                a_id,
			std::span<const FrameworkSettings::Binding> a_bindings,
			std::uint32_t                              a_current,
			Setter                                     a_set)
		{
			const auto current = std::ranges::find(
				a_bindings, a_current, &FrameworkSettings::Binding::Code);
			bool changed{};
			ImGui::TextUnformatted(a_label);
			if (ImGui::BeginCombo(
					a_id,
					current == a_bindings.end() ? "UNKNOWN" : current->Name.data())) {
				for (const auto& binding : a_bindings) {
					const bool selected = binding.Code == a_current;
					ImGui::PushID(static_cast<int>(binding.Code));
					if (ImGui::Selectable(binding.Name.data(), selected)) {
						changed = a_set(binding.Code);
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

		[[nodiscard]] bool MatchesLiveSettings(
			const FrameworkSettings::FontSettings& a_settings) noexcept
		{
			return FrameworkSettings::FontSettingsEqual(
				a_settings, FontManager::GetActiveSettings(), 0.0001F, true);
		}

		[[nodiscard]] bool QueueLiveFontSettings(
			const FrameworkSettings::FontSettings& a_settings) noexcept
		{
			return FrameworkSettings::ValidateFontSettings(a_settings) &&
			       FontManager::RequestAtlasRebuild(a_settings);
		}

		[[nodiscard]] const FontManager::FontEntry* FindFont(
			std::span<const FontManager::FontEntry> a_fonts,
			std::string_view                        a_name) noexcept
		{
			const auto found = std::ranges::find_if(a_fonts, [a_name](const auto& a_font) {
				return FrameworkSettings::EqualsIgnoreCaseAscii(a_name, a_font.Name);
			});
			return found == a_fonts.end() ? nullptr : &*found;
		}

		void FinishFontEdit(
			const FrameworkSettings::FontSettings& a_settings,
			bool                                   a_changed)
		{
			if (a_changed) {
				fontSettingsInvalid =
					!FrameworkSettings::ValidateFontSettings(a_settings);
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				fontSettingsInvalid = !QueueLiveFontSettings(a_settings);
			}
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
			const auto pendingFontName =
				FrameworkSettings::GetFontFileNameView(pending.PrimaryFont);

			const auto fonts = FontManager::GetFonts();
			const FontManager::FontEntry* pendingFont =
				FindFont(fonts, pendingFontName);
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
					const bool selected = FrameworkSettings::EqualsIgnoreCaseAscii(
						pendingFontName, name);
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::Selectable(name.c_str(), selected)) {
						if (!FrameworkSettings::CopyFontFileName(
								name, pending.PrimaryFont, false)) {
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

			if (pendingFont && pendingFont->WeightAxis) {
				const auto& axis = *pendingFont->WeightAxis;
				ImGui::Text(
					"Font weight (%.0f - %.0f)",
					axis.Minimum,
					axis.Maximum);
				const bool weightEdited = ImGui::SliderFloat(
						"##FontWeight",
						&pending.FontWeight,
						axis.Minimum,
						axis.Maximum,
						"%.0f",
						ImGuiSliderFlags_AlwaysClamp);
				FinishFontEdit(pending, weightEdited);
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
			FinishFontEdit(pending, fontSizeEdited);

			ImGui::TextUnformatted("UI scale");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip(
					"Scales text, ImGui controls, and framework layout.");
			}
			int uiScalePercent = static_cast<int>(
				std::lround(pending.UIScale * 100.0F));
			const bool uiScaleEdited = ImGui::SliderInt(
					"##UIScale",
					&uiScalePercent,
					75,
					200,
					"%d%%");
			if (uiScaleEdited) {
				pending.UIScale = static_cast<float>(uiScalePercent) / 100.0F;
			}
			FinishFontEdit(pending, uiScaleEdited);

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
			} else if (!FrameworkSettings::FontSettingsEqual(
				pending, configured, 0.0001F, true)) {
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
			const auto settingsBeforeRender = FrameworkSettings::CaptureSnapshot();
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
					"##KeyboardToggleMode",
					FrameworkSettings::GetToggleMode(),
					&FrameworkSettings::SetToggleMode) ||
				changed;
			changed = RenderBinding(
				"Toggle key (keyboard)",
				"##KeyboardToggleKey",
				FrameworkSettings::GetKeyboardBindings(),
				FrameworkSettings::GetToggleKey(),
				&FrameworkSettings::SetToggleKey) || changed;

			ImGui::Separator();
			changed =
				RenderToggleMode(
					"Toggle mode (gamepad)",
					"##GamePadToggleMode",
					FrameworkSettings::GetToggleModeGamePad(),
					&FrameworkSettings::SetToggleModeGamePad) ||
				changed;
			changed = RenderBinding(
				"Toggle key (gamepad)",
				"##GamePadToggleKey",
				FrameworkSettings::GetGamePadBindings(),
				FrameworkSettings::GetToggleKeyGamePad(),
				&FrameworkSettings::SetToggleKeyGamePad) || changed;

			if (changed) {
				ApplyRuntimeSettings();
				saveFailed = !SaveOrRestore(
					settingsBeforeRender, false, themeLoadFailed);
			}

			ImGui::Spacing();
			if (ImGui::Button("Reset to defaults")) {
				const auto settingsBeforeReset = FrameworkSettings::CaptureSnapshot();
				FrameworkSettings::ResetDefaults();
				fontSettingsInvalid = !FontManager::RequestAtlasRebuild(
					FrameworkSettings::GetFontSettings());
				fontSettingsRefreshRequested = true;
				themeLoadFailed = !ThemeManager::QueueConfiguredTheme();
				ApplyRuntimeSettings();
				saveFailed = !SaveOrRestore(
					settingsBeforeReset, true, themeLoadFailed);
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
