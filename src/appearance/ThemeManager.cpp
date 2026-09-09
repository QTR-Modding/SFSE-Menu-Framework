#include "appearance/ThemeManager.h"

#include "appearance/AssetDiscovery.h"
#include "appearance/ThemeBackdrop.h"
#include "appearance/WallpaperDrawing.h"
#include "config/FrameworkSettings.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace SFSEMenuFramework::ThemeManager
{
	using Appearance::Detail::DiscoverFiles;

	namespace
	{
		// Theme discovery, the dark-style baseline, the JSON field schema, and
		// #RRGGBBAA color behavior directly adapt SKSE Menu Framework 3
		// include/Theme.h and src/Theme.cpp at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// This port adds bounded paths, deterministic discovery, validation,
		// temporary-style parsing, and next-frame application.
		constexpr wchar_t relativeThemeDirectory[]{
			L"Data/SFSE/Plugins/SFSEMenuFrameworkThemes"
		};
		constexpr std::uintmax_t maximumThemeBytes = 1024 * 1024;
		constexpr double maximumStyleMagnitude = 10000.0;
		constexpr std::size_t NO_THEME =
			(std::numeric_limits<std::size_t>::max)();

		struct ThemeSelection final
		{
			ImGuiStyle           BaseStyle;
			ThemeBackdrop::Style Backdrop;
			std::shared_ptr<const WallpaperImage> Image;
			std::size_t          Index{ NO_THEME };
			float                UIScale{ 1.0F };
			float                BackgroundOpacity{ 1.0F };
			float                WallpaperOpacity{ 1.0F };
			float                WallpaperDimming{ 0.25F };
		};

		struct State final
		{
			std::vector<ThemeEntry> Themes;
			std::optional<ThemeSelection> Pending;
			ThemeSelection Active;
			std::uintptr_t WallpaperTexture{};
			bool WallpaperUploadFailed{};
		};

		[[nodiscard]] State& GetState()
		{
			static auto* state = new State();
			return *state;
		}

		void RefreshThemes()
		{
			auto& state = GetState();
			state.Pending.reset();
			state.Active.Index = NO_THEME;
			DiscoverFiles(relativeThemeDirectory, "theme", false, state.Themes,
				[](const std::filesystem::path& a_path, std::string& a_name) {
					if (a_path.extension() != ".json") {
						return false;
					}
					if (!FrameworkSettings::NormalizeMenuStyleName(
							a_path.stem().native(), a_name)) {
						logger::warn("Ignoring a theme with an unsupported filename");
						return false;
					}
					return true;
				});
		}

		void BuildBaselineStyle(ImGuiStyle& a_style)
		{
			ImGui::StyleColorsDark(&a_style);
			a_style.WindowRounding = 0.0F;
			a_style.FrameRounding = 0.0F;
			a_style.GrabRounding = 0.0F;
			a_style.ScrollbarRounding = 0.0F;
			a_style.ChildRounding = 0.0F;
			a_style.PopupRounding = 0.0F;
			a_style.WindowBorderSize = 1.0F;
			a_style.ChildBorderSize = 1.0F;
			a_style.FrameBorderSize = 0.0F;
			a_style.PopupBorderSize = 1.0F;
			a_style.TabBarBorderSize = 0.0F;
			a_style.TabBorderSize = 0.0F;
			a_style.FramePadding = ImVec2{ 4.0F, 3.0F };
		}

		void ScaleStyle(ImGuiStyle& a_style, float a_scale) noexcept
		{
			if (a_scale == 1.0F) {
				return;
			}

			// ScaleAllSizes intentionally truncates most pixel dimensions, but
			// truncating MouseCursorScale would turn any sub-100% setting into
			// zero. Restore that one field at its precise scaled value.
			const auto mouseCursorScale = a_style.MouseCursorScale * a_scale;
			a_style.ScaleAllSizes(a_scale);
			a_style.MouseCursorScale = mouseCursorScale;
		}

		void ApplySelection(const ThemeSelection& a_selection) noexcept
		{
			auto style = a_selection.BaseStyle;
			constexpr ImGuiCol backgroundColors[]{
				ImGuiCol_WindowBg,
				ImGuiCol_ChildBg,
				ImGuiCol_PopupBg,
				ImGuiCol_TitleBg,
				ImGuiCol_TitleBgActive,
				ImGuiCol_TitleBgCollapsed,
				ImGuiCol_MenuBarBg,
				ImGuiCol_ScrollbarBg,
				ImGuiCol_TableHeaderBg
			};
			for (const auto color : backgroundColors) {
				style.Colors[color].w *= a_selection.BackgroundOpacity;
			}
			ScaleStyle(style, a_selection.UIScale);
			ImGui::GetStyle() = style;
		}

		[[nodiscard]] const nlohmann::json* FindValue(
			const nlohmann::json& a_json, std::string_view a_key) noexcept
		{
			const auto iterator = a_json.find(a_key);
			return iterator == a_json.end() ? nullptr : &*iterator;
		}

		[[nodiscard]] bool ReadValue(
			const nlohmann::json& a_json, std::string_view a_key, bool& a_result,
			double = 0.0, double = 0.0)
		{
			const auto* value = FindValue(a_json, a_key);
			if (!value) {
				return true;
			}
			if (!value->is_boolean()) {
				return false;
			}
			a_result = value->get<bool>();
			return true;
		}

		[[nodiscard]] bool ReadValue(
			const nlohmann::json& a_json, std::string_view a_key, float& a_result,
			double a_minimum = -maximumStyleMagnitude,
			double a_maximum = maximumStyleMagnitude)
		{
			const auto* value = FindValue(a_json, a_key);
			if (!value) {
				return true;
			}
			if (!value->is_number()) {
				return false;
			}
			const auto number = value->get<double>();
			if (!std::isfinite(number) || number < a_minimum || number > a_maximum) {
				return false;
			}
			a_result = static_cast<float>(number);
			return true;
		}

		[[nodiscard]] bool ReadValue(
			const nlohmann::json& a_json, std::string_view a_key, ImVec2& a_result,
			double a_minimum = -maximumStyleMagnitude,
			double a_maximum = maximumStyleMagnitude)
		{
			const auto* value = FindValue(a_json, a_key);
			if (!value) {
				return true;
			}
			if (!value->is_array() || value->size() != 2 ||
				!(*value)[0].is_number() || !(*value)[1].is_number()) {
				return false;
			}
			const auto x = (*value)[0].get<double>();
			const auto y = (*value)[1].get<double>();
			if (!std::isfinite(x) || !std::isfinite(y) ||
				x < a_minimum || x > a_maximum ||
				y < a_minimum || y > a_maximum) {
				return false;
			}
			a_result = { static_cast<float>(x), static_cast<float>(y) };
			return true;
		}

		[[nodiscard]] bool ParseColor(
			std::string_view a_text,
			ImVec4&          a_result) noexcept
		{
			if (a_text.size() != 9 || a_text.front() != '#') {
				return false;
			}

			std::uint32_t value{};
			const auto [end, error] = std::from_chars(
				a_text.data() + 1, a_text.data() + a_text.size(), value, 16);
			if (error != std::errc{} || end != a_text.data() + a_text.size()) {
				return false;
			}

			constexpr float scale = 1.0F / 255.0F;
			a_result = ImVec4{
				static_cast<float>((value >> 24) & 0xFF) * scale,
				static_cast<float>((value >> 16) & 0xFF) * scale,
				static_cast<float>((value >> 8) & 0xFF) * scale,
				static_cast<float>(value & 0xFF) * scale
			};
			return true;
		}

		[[nodiscard]] bool ReadBackdrop(
			const nlohmann::json& a_json,
			ThemeBackdrop::Style& a_result)
		{
			const auto* backdrop = FindValue(a_json, "Backdrop");
			if (!backdrop) {
				return true;
			}
			if (!backdrop->is_object()) {
				return false;
			}

			const auto* type = FindValue(*backdrop, "Type");
			if (!type || !type->is_string()) {
				return false;
			}
			const auto& typeName = type->get_ref<const std::string&>();
			if (typeName == "None") {
				a_result = {};
				return true;
			}
			if (typeName == "Wallpaper") {
				a_result.Kind = ThemeBackdrop::Effect::Wallpaper;
				return ReadValue(*backdrop, "Opacity", a_result.ImageOpacity, 0.0, 1.0) &&
					ReadValue(*backdrop, "Darkening", a_result.ImageDarkening, 0.0, 1.0);
			}
			if (typeName != "Stars") {
				return false;
			}
			a_result.Kind = ThemeBackdrop::Effect::Stars;

			for (const auto [name, color] : {
				std::pair{ "StarColor", &ThemeBackdrop::Style::StarColor },
				std::pair{ "AccentColor", &ThemeBackdrop::Style::AccentColor } }) {
				const auto* value = FindValue(*backdrop, name);
				if (value && (!value->is_string() ||
					!ParseColor(value->get_ref<const std::string&>(), a_result.*color))) {
					return false;
				}
			}
			return ReadValue(*backdrop, "Density", a_result.Density, 0.0, 1.0);
		}

		[[nodiscard]] bool ApplyColors(
			const nlohmann::json& a_json,
			ImGuiStyle&           a_style)
		{
			const auto iterator = a_json.find("ImGuiCol");
			if (iterator == a_json.end()) {
				return true;
			}
			if (!iterator->is_object()) {
				return false;
			}

			for (const auto& [name, value] : iterator->items()) {
				if (!value.is_string()) {
					return false;
				}

				ImVec4 color{};
				if (!ParseColor(value.get_ref<const std::string&>(), color)) {
					return false;
				}

				for (int index = 0; index < ImGuiCol_COUNT; ++index) {
					if (name == ImGui::GetStyleColorName(index)) {
						a_style.Colors[index] = color;
						break;
					}
				}
			}
			return true;
		}

		template <class Value>
		struct StyleField final
		{
			std::string_view Name;
			Value ImGuiStyle::* Member;
			double Minimum{ -maximumStyleMagnitude };
			double Maximum{ maximumStyleMagnitude };
		};
		template <class Value, std::size_t Count>
		[[nodiscard]] bool ReadStyleFields(
			const nlohmann::json& a_json, ImGuiStyle& a_style,
			const StyleField<Value> (&a_fields)[Count])
		{
			for (const auto& field : a_fields) {
				if (!ReadValue(a_json, field.Name, a_style.*field.Member,
						field.Minimum, field.Maximum)) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool ApplyJsonFields(
			const nlohmann::json& a_json,
			ImGuiStyle&           a_style)
		{
			static constexpr StyleField<float> floatFields[]{
				{ "Alpha", &ImGuiStyle::Alpha, 0.0, 1.0 },
				{ "DisabledAlpha", &ImGuiStyle::DisabledAlpha, 0.0, 1.0 },
				{ "WindowRounding", &ImGuiStyle::WindowRounding },
				{ "WindowBorderSize", &ImGuiStyle::WindowBorderSize },
				{ "ChildRounding", &ImGuiStyle::ChildRounding },
				{ "ChildBorderSize", &ImGuiStyle::ChildBorderSize },
				{ "PopupRounding", &ImGuiStyle::PopupRounding },
				{ "PopupBorderSize", &ImGuiStyle::PopupBorderSize },
				{ "FrameRounding", &ImGuiStyle::FrameRounding },
				{ "FrameBorderSize", &ImGuiStyle::FrameBorderSize },
				{ "IndentSpacing", &ImGuiStyle::IndentSpacing },
				{ "ColumnsMinSpacing", &ImGuiStyle::ColumnsMinSpacing },
				{ "ScrollbarSize", &ImGuiStyle::ScrollbarSize },
				{ "ScrollbarRounding", &ImGuiStyle::ScrollbarRounding },
				{ "GrabMinSize", &ImGuiStyle::GrabMinSize },
				{ "GrabRounding", &ImGuiStyle::GrabRounding },
				{ "LogSliderDeadzone", &ImGuiStyle::LogSliderDeadzone },
				{ "TabRounding", &ImGuiStyle::TabRounding },
				{ "TabBorderSize", &ImGuiStyle::TabBorderSize },
				{
					"TabMinWidthForCloseButton",
					&ImGuiStyle::TabMinWidthForCloseButton,
					-maximumStyleMagnitude,
					static_cast<double>((std::numeric_limits<float>::max)())
				},
				{ "TabBarBorderSize", &ImGuiStyle::TabBarBorderSize },
				{ "DockingSeparatorSize", &ImGuiStyle::DockingSeparatorSize },
				{
					"TableAngledHeadersAngle",
					&ImGuiStyle::TableAngledHeadersAngle,
					-50.0,
					50.0
				},
				{ "SeparatorTextBorderSize", &ImGuiStyle::SeparatorTextBorderSize },
				{ "MouseCursorScale", &ImGuiStyle::MouseCursorScale, 0.01, 100.0 },
				{
					"CurveTessellationTol",
					&ImGuiStyle::CurveTessellationTol,
					0.01,
					maximumStyleMagnitude
				},
				{
					"CircleTessellationMaxError",
					&ImGuiStyle::CircleTessellationMaxError,
					0.01,
					maximumStyleMagnitude
				}
			};
			static constexpr StyleField<ImVec2> vectorFields[]{
				{ "WindowPadding", &ImGuiStyle::WindowPadding },
				{ "WindowMinSize", &ImGuiStyle::WindowMinSize, 1.0 },
				{ "WindowTitleAlign", &ImGuiStyle::WindowTitleAlign },
				{ "FramePadding", &ImGuiStyle::FramePadding },
				{ "ItemSpacing", &ImGuiStyle::ItemSpacing },
				{ "ItemInnerSpacing", &ImGuiStyle::ItemInnerSpacing },
				{ "CellPadding", &ImGuiStyle::CellPadding },
				{ "TouchExtraPadding", &ImGuiStyle::TouchExtraPadding },
				{ "TableAngledHeadersTextAlign", &ImGuiStyle::TableAngledHeadersTextAlign },
				{ "ButtonTextAlign", &ImGuiStyle::ButtonTextAlign },
				{ "SelectableTextAlign", &ImGuiStyle::SelectableTextAlign },
				{ "SeparatorTextAlign", &ImGuiStyle::SeparatorTextAlign },
				{ "SeparatorTextPadding", &ImGuiStyle::SeparatorTextPadding },
				{ "DisplayWindowPadding", &ImGuiStyle::DisplayWindowPadding },
				{ "DisplaySafeAreaPadding", &ImGuiStyle::DisplaySafeAreaPadding }
			};
			static constexpr StyleField<bool> boolFields[]{
				{ "AntiAliasedLines", &ImGuiStyle::AntiAliasedLines },
				{ "AntiAliasedLinesUseTex", &ImGuiStyle::AntiAliasedLinesUseTex },
				{ "AntiAliasedFill", &ImGuiStyle::AntiAliasedFill }
			};

			return ReadStyleFields(a_json, a_style, floatFields) &&
				ReadStyleFields(a_json, a_style, vectorFields) &&
				ReadStyleFields(a_json, a_style, boolFields) &&
				ApplyColors(a_json, a_style);
		}

		[[nodiscard]] bool LoadTheme(
			const ThemeEntry& a_theme,
			ThemeSelection&   a_selection)
		{
			std::error_code error;
			const auto size = std::filesystem::file_size(a_theme.Path, error);
			if (error || size == 0 || size > maximumThemeBytes) {
				logger::warn(
					"Theme '{}' has an invalid or unreadable file size",
					a_theme.Name);
				return false;
			}

			std::ifstream stream{ a_theme.Path, std::ios::binary };
			if (!stream) {
				logger::warn("Could not open theme '{}'", a_theme.Name);
				return false;
			}

			const auto json =
				nlohmann::json::parse(stream, nullptr, false, false);
			if (json.is_discarded() || !json.is_object()) {
				logger::warn("Theme '{}' is not valid JSON", a_theme.Name);
				return false;
			}

			BuildBaselineStyle(a_selection.BaseStyle);
			if (!ApplyJsonFields(json, a_selection.BaseStyle) ||
				!ReadBackdrop(json, a_selection.Backdrop)) {
				logger::warn("Theme '{}' contains an invalid style or backdrop value", a_theme.Name);
				return false;
			}
			if (a_selection.Backdrop.Kind == ThemeBackdrop::Effect::Wallpaper) {
				const auto* path = FindValue(json["Backdrop"], "Image");
				if (!path || !path->is_string() ||
					!(a_selection.Image = LoadWallpaperImage(a_theme.Path.parent_path(),
						path->get_ref<const std::string&>()))) {
					logger::warn("Theme '{}' has an unreadable or unsupported wallpaper", a_theme.Name);
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] std::size_t FindTheme(std::string_view a_name) noexcept
		{
			const auto& themes = GetState().Themes;
			for (std::size_t index = 0; index < themes.size(); ++index) {
				if (themes[index].Name == a_name) {
					return index;
				}
			}
			return NO_THEME;
		}

		[[nodiscard]] bool ApplyImmediately(std::size_t a_index)
		{
			auto& state = GetState();
			if (a_index >= state.Themes.size()) {
				return false;
			}

			ThemeSelection selection;
			if (!LoadTheme(state.Themes[a_index], selection)) {
				return false;
			}
			selection.Index = a_index;
			selection.UIScale = state.Active.UIScale;
			selection.BackgroundOpacity = state.Active.BackgroundOpacity;
			selection.WallpaperOpacity = state.Active.WallpaperOpacity;
			selection.WallpaperDimming = state.Active.WallpaperDimming;
			ApplySelection(selection);
			state.Active = std::move(selection);
			return true;
		}

		[[nodiscard]] bool TryFallbackTheme()
		{
			for (const auto fallback : { "STARFIELD", "CLASSIC" }) {
				const auto index = FindTheme(fallback);
				if (index != NO_THEME && ApplyImmediately(index)) {
					return true;
				}
			}
			return false;
		}
	}

	void Initialize()
	{
		auto& state = GetState();
		state.Active.UIScale = FrameworkSettings::GetFontSettings().UIScale;
		state.Active.BackgroundOpacity = FrameworkSettings::GetBackgroundOpacity();
		state.Active.WallpaperOpacity = FrameworkSettings::GetWallpaperOpacity();
		state.Active.WallpaperDimming = FrameworkSettings::GetWallpaperDimming();
		RefreshThemes();

		const auto configured = FrameworkSettings::GetMenuStyle();
		const auto configuredIndex = FindTheme(configured.data());
		if (configuredIndex != NO_THEME && ApplyImmediately(configuredIndex)) {
			logger::info("Applied ImGui theme '{}'", state.Themes[configuredIndex].Name);
			return;
		}

		if (configuredIndex == NO_THEME) {
			logger::warn(
				"Configured ImGui theme '{}' was not found",
				configured.data());
		}
		if (TryFallbackTheme()) {
			logger::info(
				"Applied fallback ImGui theme '{}'",
				state.Themes[state.Active.Index].Name);
			return;
		}

		BuildBaselineStyle(state.Active.BaseStyle);
		state.Active.Backdrop = {};
		state.Active.Image.reset();
		state.Active.Index = NO_THEME;
		ApplySelection(state.Active);
		logger::warn("No valid JSON theme was available; using the built-in dark style");
	}

	void ApplyPending() noexcept
	{
		auto& state = GetState();
		if (!state.Pending) {
			return;
		}

		ApplySelection(*state.Pending);
		state.Active = std::move(*state.Pending);
		state.Pending.reset();
	}

	std::span<const ThemeEntry> GetThemes() noexcept
	{
		return GetState().Themes;
	}

	std::size_t GetSelectedThemeIndex() noexcept
	{
		const auto& state = GetState();
		if (state.Pending) {
			return state.Pending->Index;
		}
		return state.Active.Index;
	}

	bool QueueTheme(std::size_t a_index)
	{
		auto& state = GetState();
		if (a_index >= state.Themes.size()) {
			return false;
		}

		ThemeSelection selection;
		if (!LoadTheme(state.Themes[a_index], selection) ||
			!FrameworkSettings::SetMenuStyle(state.Themes[a_index].Name)) {
			return false;
		}
		selection.Index = a_index;
		selection.UIScale =
			state.Pending ? state.Pending->UIScale : state.Active.UIScale;
		selection.BackgroundOpacity = FrameworkSettings::GetBackgroundOpacity();
		selection.WallpaperOpacity = FrameworkSettings::GetWallpaperOpacity();
		selection.WallpaperDimming = FrameworkSettings::GetWallpaperDimming();
		state.Pending = std::move(selection);
		return true;
	}

	bool QueueConfiguredTheme()
	{
		const auto configured = FrameworkSettings::GetMenuStyle();
		const auto index = FindTheme(configured.data());
		return index != NO_THEME && QueueTheme(index);
	}

	bool QueueUIScale(float a_scale) noexcept
	{
		if (!std::isfinite(a_scale) || a_scale < 0.75F || a_scale > 2.0F) {
			return false;
		}

		auto& state = GetState();
		auto selection = state.Pending ? *state.Pending : state.Active;
		selection.UIScale = a_scale;
		state.Pending = std::move(selection);
		return true;
	}

	namespace
	{
		bool QueueUnitValue(float a_value, float ThemeSelection::* a_member) noexcept
		{
			if (!std::isfinite(a_value) || a_value < 0.0F || a_value > 1.0F) {
				return false;
			}
			auto& state = GetState();
			auto selection = state.Pending ? *state.Pending : state.Active;
			selection.*a_member = a_value;
			state.Pending = std::move(selection);
			return true;
		}
	}

	bool QueueBackgroundOpacity(float a_value) noexcept
	{ return QueueUnitValue(a_value, &ThemeSelection::BackgroundOpacity); }
	bool QueueWallpaperOpacity(float a_value) noexcept
	{ return QueueUnitValue(a_value, &ThemeSelection::WallpaperOpacity); }
	bool QueueWallpaperDimming(float a_value) noexcept
	{ return QueueUnitValue(a_value, &ThemeSelection::WallpaperDimming); }

	bool IsWallpaperSelected() noexcept
	{
		const auto& state = GetState();
		return (state.Pending ? state.Pending->Backdrop : state.Active.Backdrop).Kind ==
			ThemeBackdrop::Effect::Wallpaper;
	}

	std::shared_ptr<const WallpaperImage> GetWallpaperImage() noexcept
	{ return GetState().Active.Image; }

	void SetWallpaperTexture(std::uintptr_t a_texture, bool a_failed) noexcept
	{
		auto& state = GetState();
		state.WallpaperTexture = a_texture;
		state.WallpaperUploadFailed = a_failed;
	}

	bool HasWallpaperUploadError() noexcept { return GetState().WallpaperUploadFailed; }

	void RenderCurrentWindowBackdrop() noexcept
	{
		const auto& state = GetState();
		const auto& active = state.Active;
		if (active.Image && state.WallpaperTexture) {
			WallpaperDrawing::RenderCurrentWindow(
				reinterpret_cast<ImTextureID>(state.WallpaperTexture),
				ImVec2{ static_cast<float>(active.Image->Width),
					static_cast<float>(active.Image->Height) },
				active.BackgroundOpacity * active.WallpaperOpacity * active.Backdrop.ImageOpacity,
				(1.0F - active.WallpaperDimming) * (1.0F - active.Backdrop.ImageDarkening));
		} else {
			ThemeBackdrop::RenderCurrentWindow(active.Backdrop, active.BackgroundOpacity);
		}
	}
}
