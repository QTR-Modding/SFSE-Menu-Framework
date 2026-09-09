#include "ui/SettingsWindow.h"

#include "appearance/FontManager.h"
#include "appearance/ThemeManager.h"
#include "config/FrameworkSettings.h"
#include "input/BindingCapture.h"
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
#include <optional>
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
		bool isOpen{};
		bool focusRequested{};
		bool fontSettingsRefreshRequested{ true };
		bool fontSettingsInvalid{};
		bool saveFailed{};
		bool themeLoadFailed{};

		struct BackgroundPreview final
		{
			float Original{};
			float Preview{};
		};
		struct BackgroundControl final
		{
			const char* Label;
			const char* Hint;
			float (*Read)() noexcept;
			bool (*Write)(float) noexcept;
			bool (*Queue)(float) noexcept;
			std::optional<BackgroundPreview> Edit;
		};
		std::array backgroundControls{
			BackgroundControl{ "Background opacity",
				"Adjusts panel backgrounds without fading text or controls.",
				FrameworkSettings::GetBackgroundOpacity, FrameworkSettings::SetBackgroundOpacity,
				ThemeManager::QueueBackgroundOpacity, {} },
			BackgroundControl{ "Wallpaper opacity",
				"Multiplies the wallpaper opacity chosen by the theme author.",
				FrameworkSettings::GetWallpaperOpacity, FrameworkSettings::SetWallpaperOpacity,
				ThemeManager::QueueWallpaperOpacity, {} },
			BackgroundControl{ "Wallpaper dimming",
				"Darkens the wallpaper for readability without dimming text or controls.",
				FrameworkSettings::GetWallpaperDimming, FrameworkSettings::SetWallpaperDimming,
				ThemeManager::QueueWallpaperDimming, {} }
		};

		bool HasBackgroundEdits() noexcept
		{
			return std::ranges::any_of(backgroundControls,
				[](const auto& control) { return control.Edit.has_value(); });
		}

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

		void FinishBackgroundEdit(BackgroundControl& a_control) noexcept
		{
			if (!a_control.Edit) {
				return;
			}
			const auto edit = *a_control.Edit;
			a_control.Edit.reset();
			if (!a_control.Write(edit.Preview)) {
				themeLoadFailed = true;
				static_cast<void>(a_control.Queue(edit.Original));
				return;
			}
			if (FrameworkSettings::Save()) {
				saveFailed = false;
				return;
			}
			static_cast<void>(a_control.Write(edit.Original));
			themeLoadFailed = !a_control.Queue(edit.Original);
			saveFailed = true;
		}

		void RenderBackgroundControls()
		{
			for (std::size_t index = 0; index < backgroundControls.size(); ++index) {
				auto& control = backgroundControls[index];
				if (index != 0 && !ThemeManager::IsWallpaperSelected()) {
					FinishBackgroundEdit(control);
					continue;
				}
				const auto configured = control.Read();
				const auto displayed = control.Edit ? control.Edit->Preview : configured;
				int percentage = static_cast<int>(std::lround(displayed * 100.0F));
				ImGui::TextUnformatted(control.Label);
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip("%s", control.Hint);
				}
				ImGui::PushID(control.Label);
				if (ImGui::SliderInt("##Value", &percentage, 0, 100, "%d%%",
					ImGuiSliderFlags_AlwaysClamp)) {
					const auto preview = static_cast<float>(percentage) / 100.0F;
					themeLoadFailed = !control.Queue(preview);
					if (!themeLoadFailed) {
						if (!control.Edit) {
							control.Edit = BackgroundPreview{ configured, preview };
						} else {
							control.Edit->Preview = preview;
						}
					}
				}
				if (ImGui::IsItemDeactivatedAfterEdit()) {
					FinishBackgroundEdit(control);
				}
				ImGui::PopID();
			}
			if (ThemeManager::HasWallpaperUploadError()) {
				ImGui::TextColored(ImVec4{ 1.0F, 0.4F, 0.4F, 1.0F },
					"Could not upload the wallpaper. Reselect the theme to retry.");
			}
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

		// The press-to-bind controls and warnings adapt the approved
		// QTR-Modding/SKSE-Menu-Framework-3 user-authored commits 5b269fa through
		// e297bbd (GPL-3.0). Text remains embedded English in this port.
		struct PendingToggleChange final
		{
			BindingCapture::Device       Device{ BindingCapture::Device::Keyboard };
			std::uint32_t                Key{ BindingCapture::unboundKey };
			FrameworkSettings::ToggleMode Mode{ FrameworkSettings::ToggleMode::SinglePress };
			const char*                  Warning{ nullptr };
			bool                         OpenRequested{};
		};

		PendingToggleChange pendingToggleChange;

		[[nodiscard]] std::string_view GetBindingName(
			BindingCapture::Device a_device, std::uint32_t a_key) noexcept
		{
			const auto name = a_device == BindingCapture::Device::Keyboard ?
				FrameworkSettings::GetKeyboardBindingName(a_key) :
				FrameworkSettings::GetGamePadBindingName(a_key);
			return name.empty() ? std::string_view{ "UNKNOWN" } : name;
		}

		void SaveToggleChange(
			BindingCapture::Device       a_device,
			std::uint32_t                a_key,
			FrameworkSettings::ToggleMode a_mode,
			bool&                        a_saveFailed,
			bool&                        a_themeLoadFailed)
		{
			const auto previous = FrameworkSettings::CaptureSnapshot();
			auto updated = previous;
			if (a_device == BindingCapture::Device::Keyboard) {
				updated.ToggleKey = a_key;
				updated.Mode = a_mode;
			} else {
				updated.ToggleKeyGamePad = a_key;
				updated.ModeGamePad = a_mode;
			}
			FrameworkSettings::RestoreSnapshot(updated);
			ApplyRuntimeSettings();
			a_saveFailed = !SaveOrRestore(
				previous, false, a_themeLoadFailed);
		}

		void RequestToggleChange(
			BindingCapture::Device       a_device,
			std::uint32_t                a_key,
			FrameworkSettings::ToggleMode a_mode,
			bool&                        a_saveFailed,
			bool&                        a_themeLoadFailed)
		{
			const auto current = FrameworkSettings::CaptureSnapshot();
			const bool keyboard = a_device == BindingCapture::Device::Keyboard;
			const auto currentKey = keyboard ?
				current.ToggleKey : current.ToggleKeyGamePad;
			const auto currentMode = keyboard ?
				current.Mode : current.ModeGamePad;
			if (a_key == currentKey && a_mode == currentMode) {
				return;
			}

			const char* warning{};
			if ((a_key == BindingCapture::unboundKey && a_key != currentKey) ||
				(a_mode == FrameworkSettings::ToggleMode::Off &&
				 a_mode != currentMode)) {
				warning =
					"This device will have no enabled shortcut.\n"
					"Make sure you have another way to reopen this menu.";
			} else if (!keyboard && (a_key == 4096 || a_key == 8192) &&
				(a_key != currentKey ||
				 (currentMode == FrameworkSettings::ToggleMode::Off &&
				  a_mode != currentMode))) {
				warning = a_key == 4096 ?
					"A is also the controller's Confirm button.\n"
					"While this shortcut is enabled, selecting an item will close this menu." :
					"B is also the controller's Back/Cancel button.\n"
					"With this shortcut enabled, it closes the entire MCP before\n"
					"the normal cancel or focused-window close behavior.";
			}

			if (warning) {
				pendingToggleChange = {
					a_device, a_key, a_mode, warning, true
				};
				BindingCapture::BeginConfirmation();
				return;
			}
			SaveToggleChange(
				a_device, a_key, a_mode, a_saveFailed, a_themeLoadFailed);
		}

		void RenderBindingConfirmation(
			bool& a_saveFailed, bool& a_themeLoadFailed)
		{
			constexpr char title[]{ "Change Shortcut?###BindingWarning" };
			if (pendingToggleChange.OpenRequested) {
				ImGui::OpenPopup(title);
				pendingToggleChange.OpenRequested = false;
			}
			if (!ImGui::BeginPopupModal(
					title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				return;
			}
			if (!BindingCapture::IsConfirming()) {
				BindingCapture::Acknowledge();
				pendingToggleChange = {};
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}

			ImGui::TextUnformatted(pendingToggleChange.Warning);
			ImGui::Separator();
			if (ImGui::IsWindowAppearing()) {
				ImGui::NavRestoreHighlightAfterMove();
			}
			const bool cancel = ImGui::Button("Cancel");
			ImGui::SetItemDefaultFocus();
			ImGui::SameLine();
			const bool confirm = ImGui::Button("Change Anyway");
			if (cancel || confirm) {
				if (confirm && !cancel) {
					SaveToggleChange(
						pendingToggleChange.Device,
						pendingToggleChange.Key,
						pendingToggleChange.Mode,
						a_saveFailed,
						a_themeLoadFailed);
				}
				BindingCapture::Acknowledge();
				pendingToggleChange = {};
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		void RenderKeyBinding(
			std::uint32_t                 a_binding,
			BindingCapture::Device        a_device,
			FrameworkSettings::ToggleMode a_mode,
			bool&                         a_saveFailed,
			bool&                         a_themeLoadFailed)
		{
			ImGui::PushID("ToggleKey");
			constexpr char title[]{ "Set Binding###CaptureBinding" };
			constexpr char clearLabel[]{ "Clear" };
			const auto clearWidth =
				ImGui::CalcTextSize(clearLabel).x +
				ImGui::GetStyle().FramePadding.x * 2.0F;
			const auto bindingWidth = (std::max)(
				ImGui::CalcItemWidth() - clearWidth -
					ImGui::GetStyle().ItemSpacing.x,
				ImGui::GetFrameHeight());
			const auto name = GetBindingName(a_device, a_binding);
			if (ImGui::Button(
					name.data(), ImVec2{ bindingWidth, 0.0F })) {
				BindingCapture::Begin(a_device);
				ImGui::OpenPopup(title);
			}
			ImGui::SameLine();
			if (ImGui::Button(clearLabel) &&
				a_binding != BindingCapture::unboundKey) {
				RequestToggleChange(
					a_device, BindingCapture::unboundKey, a_mode,
					a_saveFailed, a_themeLoadFailed);
			}

			if (ImGui::BeginPopupModal(
					title, nullptr,
					ImGuiWindowFlags_AlwaysAutoResize |
						ImGuiWindowFlags_NoNavInputs)) {
				auto newKey = a_binding;
				auto state = BindingCapture::Poll(newKey, a_device);
				ImGui::TextUnformatted(
					a_device == BindingCapture::Device::Keyboard ?
						"Press and release a key. Escape cancels." :
						"Press and release a controller button. Escape cancels.");
				ImGui::BeginDisabled(state == BindingCapture::State::Pressed);
				if (ImGui::Button("Cancel")) {
					state = BindingCapture::State::Cancelled;
				}
				ImGui::EndDisabled();
				if (state == BindingCapture::State::Idle ||
					state == BindingCapture::State::Complete ||
					state == BindingCapture::State::Cancelled) {
					BindingCapture::Acknowledge();
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
				if (state == BindingCapture::State::Complete) {
					RequestToggleChange(
						a_device, newKey, a_mode,
						a_saveFailed, a_themeLoadFailed);
				}
			}
			ImGui::PopID();
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
				GlyphToggle{ "Polish", &FrameworkSettings::GlyphCoverage::Polish },
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

			RenderBackgroundControls();

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

			const auto inputDevice = BindingCapture::GetActiveDevice();
			const bool keyboard =
				inputDevice == BindingCapture::Device::Keyboard;
			const auto binding = keyboard ?
				edited.ToggleKey : edited.ToggleKeyGamePad;
			auto toggleMode = keyboard ?
				edited.Mode : edited.ModeGamePad;
			ImGui::Separator();
			if (RenderToggleMode(
					"Toggle mode", "##ToggleMode", toggleMode)) {
				RequestToggleChange(
					inputDevice, binding, toggleMode,
					saveFailed, themeLoadFailed);
			}
			ImGui::Separator();
			ImGui::TextUnformatted("Toggle key");
			RenderKeyBinding(
				binding, inputDevice, toggleMode,
				saveFailed, themeLoadFailed);
			RenderBindingConfirmation(saveFailed, themeLoadFailed);

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
					"Could not apply the selected appearance");
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

	void UpdateLifecycle() noexcept
	{
		static std::uint64_t observedMainSessionGeneration{};
		const auto generation =
			WindowManager::GetMainWindowSessionGeneration();
		if (generation == 0) {
			if (isOpen || HasBackgroundEdits()) {
				Close();
			}
			return;
		}
		if (generation != observedMainSessionGeneration) {
			observedMainSessionGeneration = generation;
			Close();
		}
	}

	void Open() noexcept
	{
		isOpen = true;
		focusRequested = true;
	}

	void Close() noexcept
	{
		for (auto& control : backgroundControls) {
			FinishBackgroundEdit(control);
		}
		BindingCapture::Acknowledge();
		pendingToggleChange = {};
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
		if (drawContents) {
			ThemeManager::RenderCurrentWindowBackdrop();
		}
		const bool closeRequested =
			GamepadNavigation::ConsumeCloseRequestForCurrentWindow(
				WindowManager::GetBlockingWindowOpenGeneration());
		if (closeRequested) {
			Close();
		}
		if (!closeRequested && drawContents && ImGui::BeginMenuBar()) {
			ImGui::TextUnformatted("Settings");
			if (SFSEMenuFramework::UI::RenderCloseButton()) {
				Close();
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
