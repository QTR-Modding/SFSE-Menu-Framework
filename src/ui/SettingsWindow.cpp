#include "ui/SettingsWindow.h"

#include "appearance/FontManager.h"
#include "appearance/ThemeManager.h"
#include "config/FrameworkSettings.h"
#include "input/GamepadNavigation.h"
#include "platform/win32/Win32Platform.h"
#include "runtime/WindowManager.h"
#include "ui/WindowPlacement.h"
#include "ui/WindowPresentation.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SFSEMenuFramework::SettingsWindow
{
	namespace
	{
		// The separate settings-window presentation and toggle control directly
		// adapt SKSE Menu Framework 3 UI.cpp at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// Persistence and Starfield ownership updates remain SFSE-specific.
		bool isOpen{};
		bool focusRequested{};
		bool fontSettingsRefreshRequested{ true };
		bool fontSettingsInvalid{};

		void ApplyRuntimeSettings()
		{
			if (auto* mainWindow = WindowManager::GetMainWindow()) {
				mainWindow->PauseGame.store(
					FrameworkSettings::GetFreezeTimeOnMenu(), std::memory_order_release);
				mainWindow->BlurBackground.store(
					FrameworkSettings::GetBlurBackgroundOnMenu(), std::memory_order_release);
				static_cast<void>(Win32Platform::PostHostWindowCallback());
			}
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

		[[nodiscard]] bool RenderToggleMode(
			const char* a_label, const char* a_id,
			FrameworkSettings::ToggleMode& a_current)
		{
			int selected = std::to_underlying(a_current);
			constexpr std::array names{ "SINGLEPRESS", "HOLD", "DOUBLEPRESS", "OFF" };
			ImGui::TextUnformatted(a_label);
			if (!ImGui::Combo(a_id, &selected, names.data(), static_cast<int>(names.size()))) {
				return false;
			}
			a_current = static_cast<FrameworkSettings::ToggleMode>(selected);
			return true;
		}

		[[nodiscard]] bool RenderBinding(
			const char* a_label, const char* a_id,
			std::span<const FrameworkSettings::Binding> a_bindings,
			std::uint32_t& a_current)
		{
			const auto current = std::ranges::find(
				a_bindings, a_current, &FrameworkSettings::Binding::Code);
			bool changed{};
			ImGui::TextUnformatted(a_label);
			if (ImGui::BeginCombo(
					a_id, current == a_bindings.end() ? "UNKNOWN" : current->Name.data())) {
				for (const auto& binding : a_bindings) {
					const bool selected = binding.Code == a_current;
					ImGui::PushID(static_cast<int>(binding.Code));
					if (ImGui::Selectable(binding.Name.data(), selected)) {
						a_current = binding.Code;
						changed = true;
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
			constexpr ImVec4 transparent{};
			ImGui::PushStyleColor(ImGuiCol_Header, transparent);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
			const bool clicked = ImGui::Selectable(
				"##toggle", false, ImGuiSelectableFlags_None, ImVec2{ width, height });
			ImGui::PopStyleColor(3);
			ImGui::PopID();
			if (clicked) {
				*a_value = !*a_value;
			}

			const float offset = *a_value ? 1.0F : 0.0F;
			const auto background =
				ImGui::GetColorU32(*a_value ? ImGuiCol_ButtonActive : ImGuiCol_Button);
			drawList->AddRectFilled(
				position, ImVec2{ position.x + width, position.y + height },
				background, height * 0.5F);
			drawList->AddCircleFilled(
				ImVec2{
					position.x + radius + offset * (width - radius * 2.0F),
					position.y + radius
				},
				radius - 1.5F, IM_COL32(255, 255, 255, 255));

			ImGui::SameLine();
			ImGui::TextUnformatted(a_label);
			return clicked;
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
				fontSettingsInvalid = !FontManager::RequestAtlasRebuild(a_settings);
			}
		}

		void RenderGlyphCoverage(
			FrameworkSettings::FontSettings& a_pending)
		{
			struct GlyphToggle final
			{
				const char* Label;
				bool FrameworkSettings::GlyphCoverage::* Value;
			};
			constexpr std::array toggles{
				GlyphToggle{ "Greek", &FrameworkSettings::GlyphCoverage::Greek },
				GlyphToggle{ "Cyrillic", &FrameworkSettings::GlyphCoverage::Cyrillic },
				GlyphToggle{ "Vietnamese", &FrameworkSettings::GlyphCoverage::Vietnamese },
				GlyphToggle{ "Turkish", &FrameworkSettings::GlyphCoverage::Turkish },
				GlyphToggle{ "Thai", &FrameworkSettings::GlyphCoverage::Thai },
				GlyphToggle{ "Korean", &FrameworkSettings::GlyphCoverage::Korean },
				GlyphToggle{ "Japanese", &FrameworkSettings::GlyphCoverage::Japanese },
				GlyphToggle{
					"Chinese (Simplified Common)",
					&FrameworkSettings::GlyphCoverage::ChineseSimplifiedCommon },
				GlyphToggle{
					"Chinese (Full)",
					&FrameworkSettings::GlyphCoverage::ChineseFull }
			};

			ImGui::SeparatorText("Glyph coverage");
			ImGui::TextDisabled(
				"Enable only the character sets needed by framework panels.");
			for (const auto& toggle : toggles) {
				auto& enabled = a_pending.Glyphs.*toggle.Value;
				if (!ToggleButton(toggle.Label, &enabled)) {
					continue;
				}
				if (enabled &&
					toggle.Value ==
						&FrameworkSettings::GlyphCoverage::ChineseSimplifiedCommon) {
					a_pending.Glyphs.ChineseFull = false;
				} else if (enabled &&
					toggle.Value == &FrameworkSettings::GlyphCoverage::ChineseFull) {
					a_pending.Glyphs.ChineseSimplifiedCommon = false;
				}
				fontSettingsInvalid =
					!FontManager::RequestAtlasRebuild(a_pending);
			}
			ImGui::TextDisabled(
				"The two Chinese ranges are alternatives; Full uses substantially more atlas space.");
		}

		void RenderFontSettings(bool& a_saveFailed)
		{
			static FrameworkSettings::FontSettings pending{};
			const auto active = FontManager::GetActiveInfo();
			if (fontSettingsRefreshRequested) {
				pending = active.Settings;
				fontSettingsRefreshRequested = false;
				fontSettingsInvalid = false;
			}

			ImGui::SeparatorText("Fonts");
			ImGui::TextUnformatted("Font rendering");
			constexpr std::array renderingNames{ "NATIVE", "LIGHT", "AUTO" };
			int rendering = std::to_underlying(pending.Rendering);
			if (ImGui::Combo("##FontRendering", &rendering, renderingNames.data(),
					static_cast<int>(renderingNames.size()))) {
				pending.Rendering = static_cast<FrameworkSettings::FontRendering>(rendering);
				fontSettingsInvalid = !FontManager::RequestAtlasRebuild(pending);
			}
			switch (pending.Rendering) {
			case FrameworkSettings::FontRendering::Light:
				ImGui::TextDisabled(
					"Uses FreeType's light target; often smoother, sometimes softer.");
				break;
			case FrameworkSettings::FontRendering::Auto:
				ImGui::TextDisabled("FreeType auto-hinting; useful for weakly hinted fonts.");
				break;
			case FrameworkSettings::FontRendering::Native:
			default:
				ImGui::TextDisabled(
					"Prefers the font's built-in hinter; FreeType may fall back to auto.");
				break;
			}

			ImGui::TextUnformatted("Primary font");
			const auto pendingFontName = FrameworkSettings::GetFontFileNameView(pending.PrimaryFont);

			const auto fonts = FontManager::GetFonts();
			const auto foundFont = std::ranges::find_if(fonts, [pendingFontName](const auto& font) {
				return FrameworkSettings::EqualsIgnoreCaseAscii(pendingFontName, font.Name);
			});
			const FontManager::FontEntry* pendingFont =
				foundFont == fonts.end() ? nullptr : &*foundFont;
			std::string unavailablePreview = pendingFontName.empty() ?
				"INVALID" : "<missing: " + std::string{ pendingFontName } + ">";
			const char* fontPreview = pendingFont ? pending.PrimaryFont.data() : unavailablePreview.c_str();

			if (fonts.empty()) {
				ImGui::BeginDisabled();
				ImGui::Button("No fonts found##PrimaryFont");
				ImGui::EndDisabled();
			} else if (ImGui::BeginCombo("##PrimaryFont", fontPreview)) {
				for (std::size_t index = 0; index < fonts.size(); ++index) {
					const auto& name = fonts[index].Name;
					const bool selected =
						FrameworkSettings::EqualsIgnoreCaseAscii(pendingFontName, name);
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::Selectable(name.c_str(), selected)) {
						if (!FrameworkSettings::CopyFontFileName(name, pending.PrimaryFont, false)) {
							fontSettingsInvalid = true;
						} else {
							pendingFont = &fonts[index];
							const auto weightAxis = FontManager::GetWeightAxis(index);
							if (weightAxis) {
								pending.FontWeight = std::clamp(
									pending.FontWeight, weightAxis->Minimum, weightAxis->Maximum);
							}
							fontSettingsInvalid = !FontManager::RequestAtlasRebuild(pending);
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
				ImGui::Text("Font weight (%.0f - %.0f)", axis.Minimum, axis.Maximum);
				const bool weightEdited = ImGui::SliderFloat(
					"##FontWeight", &pending.FontWeight, axis.Minimum, axis.Maximum,
					"%.0f", ImGuiSliderFlags_AlwaysClamp);
				FinishFontEdit(pending, weightEdited);
				ImGui::TextDisabled(
					"Variable font; built-in default weight %.0f.", axis.Default);
			} else if (pendingFont) {
				ImGui::TextDisabled("Font weight: fixed by this font file.");
			} else {
				ImGui::TextDisabled("Font weight: unavailable until the font is found.");
			}

			ImGui::Text("Font size (%.0f - %.0f)", pending.MinFontSize, pending.MaxFontSize);
			const bool fontSizeEdited = ImGui::InputFloat(
				"##FontSizeMedium", &pending.FontSizeMedium, 1.0F, 4.0F, "%.1f");
			FinishFontEdit(pending, fontSizeEdited);

			ImGui::TextUnformatted("UI scale");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Scales text, ImGui controls, and framework layout.");
			}
			int uiScalePercent = static_cast<int>(std::lround(pending.UIScale * 100.0F));
			const bool uiScaleEdited = ImGui::SliderInt(
				"##UIScale", &uiScalePercent, 75, 200, "%d%%");
			if (uiScaleEdited) {
				pending.UIScale = static_cast<float>(uiScalePercent) / 100.0F;
			}
			FinishFontEdit(pending, uiScaleEdited);

			RenderGlyphCoverage(pending);

			const auto activeScalePercent =
				static_cast<int>(std::lround(active.Settings.UIScale * 100.0F));
			const auto activeRendering = FrameworkSettings::GetFontRenderingName(
				active.Settings.Rendering);
			ImGui::TextDisabled("Rendering: %.*s",
				static_cast<int>(activeRendering.size()), activeRendering.data());
			if (active.WeightAxis) {
				ImGui::TextDisabled(
					"Active: %.*s | weight %.0f | %.1f px | %d%% | %.1f raster px",
					static_cast<int>(active.Name.size()), active.Name.data(),
					active.Settings.FontWeight, active.Settings.FontSizeMedium,
					activeScalePercent, active.RasterSize);
			} else {
				ImGui::TextDisabled(
					"Active: %.*s | %.1f px | %d%% | %.1f raster px",
					static_cast<int>(active.Name.size()), active.Name.data(),
					active.Settings.FontSizeMedium, activeScalePercent, active.RasterSize);
			}
			if (!active.FallbackReason.empty()) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.75F, 0.25F, 1.0F }, "Fallback: %.*s",
					static_cast<int>(active.FallbackReason.size()), active.FallbackReason.data());
			}

			const bool pendingValid = FrameworkSettings::ValidateFontSettings(pending);
			const auto matchesLive = [&] {
				return FrameworkSettings::FontSettingsEqual(
					pending, active.Settings, 0.0001F, true);
			};
			const bool canSave = pendingValid && !fontSettingsInvalid &&
				!FontManager::HasPendingAtlasRebuild() &&
				matchesLive();
			ImGui::BeginDisabled(!canSave);
			if (ImGui::Button("Save")) {
				const auto previous = FrameworkSettings::GetFontSettings();
				if (!FrameworkSettings::SetFontSettings(pending)) {
					fontSettingsInvalid = true;
				} else if (!FrameworkSettings::Save()) {
					static_cast<void>(FrameworkSettings::SetFontSettings(previous));
					a_saveFailed = true;
				} else {
					pending = FrameworkSettings::GetFontSettings();
					fontSettingsInvalid = false;
					a_saveFailed = false;
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Reset font settings")) {
				pending = FrameworkSettings::GetDefaultFontSettings();
				fontSettingsInvalid = !FontManager::RequestAtlasRebuild(pending);
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
					ImVec4{ 0.35F, 0.8F, 1.0F, 1.0F }, "Applying live font changes...");
			} else if (!applyError.empty()) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F }, "%.*s",
					static_cast<int>(applyError.size()), applyError.data());
			} else if (pendingValid && !matchesLive()) {
				ImGui::TextDisabled("Finish editing to apply the live preview.");
			} else if (!FrameworkSettings::FontSettingsEqual(
				pending, configured, 0.0001F, true)) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.75F, 0.25F, 1.0F },
					"Live preview applied; changes are not saved.");
			} else {
				ImGui::TextDisabled(
					"Font rendering, face, weight, scale, and glyph coverage apply live.");
			}
		}

		void RenderFrameworkSettings()
		{
			static bool saveFailed{};
			static bool themeLoadFailed{};
			const auto settingsBeforeRender = FrameworkSettings::CaptureSnapshot();
			bool themeChanged{};

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
							themeChanged = true;
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
			auto edited = FrameworkSettings::CaptureSnapshot();
			bool changed{};

			struct ToggleSetting final
			{
				const char* Label;
				bool FrameworkSettings::SettingsSnapshot::* Value;
			};
			constexpr std::array toggles{
				ToggleSetting{ "Freeze time while menu is open", &FrameworkSettings::SettingsSnapshot::FreezeTimeOnMenu },
				ToggleSetting{ "Blur background while menu is open", &FrameworkSettings::SettingsSnapshot::BlurBackgroundOnMenu }
			};
			for (const auto& toggle : toggles) {
				changed = ToggleButton(toggle.Label, &(edited.*toggle.Value)) || changed;
			}

			struct InputSetting final
			{
				const char* ModeLabel;
				const char* ModeID;
				FrameworkSettings::ToggleMode FrameworkSettings::SettingsSnapshot::* Mode;
				const char* KeyLabel;
				const char* KeyID;
				std::span<const FrameworkSettings::Binding> Bindings;
				std::uint32_t FrameworkSettings::SettingsSnapshot::* Key;
			};
			const std::array inputs{
				InputSetting{ "Toggle mode (keyboard)", "##KeyboardToggleMode",
					&FrameworkSettings::SettingsSnapshot::Mode, "Toggle key (keyboard)",
					"##KeyboardToggleKey", FrameworkSettings::GetKeyboardBindings(),
					&FrameworkSettings::SettingsSnapshot::ToggleKey },
				InputSetting{ "Toggle mode (gamepad)", "##GamePadToggleMode",
					&FrameworkSettings::SettingsSnapshot::ModeGamePad, "Toggle key (gamepad)",
					"##GamePadToggleKey", FrameworkSettings::GetGamePadBindings(),
					&FrameworkSettings::SettingsSnapshot::ToggleKeyGamePad }
			};
			for (const auto& input : inputs) {
				ImGui::Separator();
				changed = RenderToggleMode(
					input.ModeLabel, input.ModeID, edited.*input.Mode) || changed;
				changed = RenderBinding(
					input.KeyLabel, input.KeyID, input.Bindings, edited.*input.Key) || changed;
			}

			if (changed || themeChanged) {
				if (changed) {
					FrameworkSettings::RestoreSnapshot(edited);
				}
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

	void Render()
	{
		if (!isOpen) {
			return;
		}

		WindowPlacement::Apply(WindowPlacement::BuiltInWindow::Settings);
		if (focusRequested) {
			ImGui::SetNextWindowFocus();
			focusRequested = false;
		}

		constexpr ImGuiWindowFlags windowFlags =
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_MenuBar |
			ImGuiWindowFlags_NoTitleBar |
			ImGuiWindowFlags_NoSavedSettings;
		const bool drawContents = ImGui::Begin(
			WindowPlacement::GetName(WindowPlacement::BuiltInWindow::Settings),
			nullptr, windowFlags);
		WindowPlacement::Capture(WindowPlacement::BuiltInWindow::Settings);
		const bool closeRequested =
			GamepadNavigation::ConsumeCloseRequestForCurrentWindow(
				WindowManager::GetBlockingWindowOpenGeneration());
		if (closeRequested) {
			isOpen = false;
		}
		if (!closeRequested && drawContents && ImGui::BeginMenuBar()) {
			ImGui::TextUnformatted("Settings");
			if (SFSEMenuFramework::UI::RenderCloseButton()) {
				isOpen = false;
			}
			ImGui::EndMenuBar();
		}

		if (!closeRequested && drawContents) {
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
