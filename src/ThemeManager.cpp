#include "ThemeManager.h"

#include "FrameworkSettings.h"

#include <Windows.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace SFSEMenuFramework::ThemeManager
{
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
		constexpr std::size_t pathCapacity = 32768;
		constexpr std::uintmax_t maximumThemeBytes = 1024 * 1024;
		constexpr double maximumStyleMagnitude = 10000.0;
		constexpr std::size_t NO_THEME =
			(std::numeric_limits<std::size_t>::max)();

		struct ThemeSelection final
		{
			ImGuiStyle  BaseStyle;
			std::size_t Index{ NO_THEME };
			float       UIScale{ 1.0F };
		};

		struct State final
		{
			std::vector<ThemeEntry> Themes;
			std::optional<ThemeSelection> Pending;
			ThemeSelection Active;
		};

		[[nodiscard]] State& GetState()
		{
			static auto* state = new State();
			return *state;
		}

		[[nodiscard]] std::filesystem::path BuildThemeDirectory()
		{
			std::array<wchar_t, pathCapacity> executablePath{};
			const auto length = ::GetModuleFileNameW(
				nullptr,
				executablePath.data(),
				static_cast<DWORD>(executablePath.size()));
			if (length == 0 || length >= executablePath.size()) {
				return {};
			}

			auto directory =
				std::filesystem::path(executablePath.data()).parent_path();
			directory /= relativeThemeDirectory;
			return directory;
		}

		[[nodiscard]] bool NormalizeThemeName(
			std::wstring_view a_name,
			std::string&     a_result)
		{
			if (a_name.empty() ||
				a_name.size() >= FrameworkSettings::MenuStyleName{}.size() ||
				a_name == L"." || a_name == L".." || a_name.front() == L' ' ||
				a_name.back() == L'.' || a_name.back() == L' ') {
				return false;
			}

			a_result.clear();
			a_result.reserve(a_name.size());
			for (const auto character : a_name) {
				const auto value = static_cast<std::uint32_t>(character);
				if (value < 0x20 || value > 0x7E || character == L'<' ||
					character == L'>' || character == L':' || character == L'"' ||
					character == L'/' || character == L'\\' || character == L'|' ||
					character == L'?' || character == L'*') {
					return false;
				}
				a_result.push_back(
					character >= 'a' && character <= 'z' ?
						static_cast<char>(character - ('a' - 'A')) :
						static_cast<char>(character));
			}
			return true;
		}

		void RefreshThemes()
		{
			auto& state = GetState();
			state.Themes.clear();
			state.Pending.reset();
			state.Active.Index = NO_THEME;

			const auto directory = BuildThemeDirectory();
			if (directory.empty()) {
				logger::warn("Could not resolve the SFSE Menu Framework theme directory");
				return;
			}

			std::error_code error;
			std::filesystem::directory_iterator iterator{ directory, error };
			const std::filesystem::directory_iterator end{};
			if (error) {
				logger::warn(
					"Could not enumerate the SFSE Menu Framework theme directory: {}",
					error.message());
				return;
			}

			for (; iterator != end; iterator.increment(error)) {
				if (error) {
					logger::warn(
						"Theme directory enumeration stopped: {}",
						error.message());
					break;
				}

				std::error_code entryError;
				if (!iterator->is_regular_file(entryError) || entryError) {
					continue;
				}

				const auto& path = iterator->path();
				if (path.extension() != ".json") {
					continue;
				}

				std::string name;
				if (!NormalizeThemeName(path.stem().native(), name)) {
					logger::warn("Ignoring a theme with an unsupported filename");
					continue;
				}
				state.Themes.push_back(ThemeEntry{
					.Name = std::move(name),
					.Path = path
				});
			}

			std::sort(
				state.Themes.begin(),
				state.Themes.end(),
				[](const ThemeEntry& a_left, const ThemeEntry& a_right) {
					if (a_left.Name != a_right.Name) {
						return a_left.Name < a_right.Name;
					}
					return a_left.Path.native() < a_right.Path.native();
				});
			state.Themes.erase(
				std::unique(
					state.Themes.begin(),
					state.Themes.end(),
					[](const ThemeEntry& a_left, const ThemeEntry& a_right) {
						return a_left.Name == a_right.Name;
					}),
				state.Themes.end());
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
			ScaleStyle(style, a_selection.UIScale);
			ImGui::GetStyle() = style;
		}

		[[nodiscard]] bool ReadFloat(
			const nlohmann::json& a_json,
			std::string_view      a_key,
			float&                a_result,
			double                a_minimum = -maximumStyleMagnitude,
			double                a_maximum = maximumStyleMagnitude)
		{
			const auto iterator = a_json.find(a_key);
			if (iterator == a_json.end()) {
				return true;
			}
			if (!iterator->is_number()) {
				return false;
			}

			const auto value = iterator->get<double>();
			if (!std::isfinite(value) || value < a_minimum || value > a_maximum) {
				return false;
			}
			a_result = static_cast<float>(value);
			return true;
		}

		[[nodiscard]] bool ReadVector(
			const nlohmann::json& a_json,
			std::string_view      a_key,
			ImVec2&               a_result,
			double                a_minimum = -maximumStyleMagnitude,
			double                a_maximum = maximumStyleMagnitude)
		{
			const auto iterator = a_json.find(a_key);
			if (iterator == a_json.end()) {
				return true;
			}
			if (!iterator->is_array() || iterator->size() != 2 ||
				!(*iterator)[0].is_number() || !(*iterator)[1].is_number()) {
				return false;
			}

			const auto x = (*iterator)[0].get<double>();
			const auto y = (*iterator)[1].get<double>();
			if (!std::isfinite(x) || !std::isfinite(y) || x < a_minimum ||
				x > a_maximum || y < a_minimum || y > a_maximum) {
				return false;
			}
			a_result = ImVec2{ static_cast<float>(x), static_cast<float>(y) };
			return true;
		}

		[[nodiscard]] bool ReadBool(
			const nlohmann::json& a_json,
			std::string_view      a_key,
			bool&                 a_result)
		{
			const auto iterator = a_json.find(a_key);
			if (iterator == a_json.end()) {
				return true;
			}
			if (!iterator->is_boolean()) {
				return false;
			}
			a_result = iterator->get<bool>();
			return true;
		}

		[[nodiscard]] int HexDigit(char a_character) noexcept
		{
			if (a_character >= '0' && a_character <= '9') {
				return a_character - '0';
			}
			if (a_character >= 'A' && a_character <= 'F') {
				return a_character - 'A' + 10;
			}
			if (a_character >= 'a' && a_character <= 'f') {
				return a_character - 'a' + 10;
			}
			return -1;
		}

		[[nodiscard]] bool ParseColor(
			std::string_view a_text,
			ImVec4&          a_result) noexcept
		{
			if (a_text.size() != 9 || a_text.front() != '#') {
				return false;
			}

			std::uint32_t value{};
			for (std::size_t index = 1; index < a_text.size(); ++index) {
				const auto digit = HexDigit(a_text[index]);
				if (digit < 0) {
					return false;
				}
				value = (value << 4) | static_cast<std::uint32_t>(digit);
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

		[[nodiscard]] bool ApplyJsonFields(
			const nlohmann::json& a_json,
			ImGuiStyle&           a_style)
		{
			struct FloatField final
			{
				std::string_view Name;
				float ImGuiStyle::* Value;
				double Minimum{ -maximumStyleMagnitude };
				double Maximum{ maximumStyleMagnitude };
			};
			static constexpr FloatField floatFields[]{
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
				{ "TabMinWidthForCloseButton", &ImGuiStyle::TabMinWidthForCloseButton,
					-maximumStyleMagnitude,
					static_cast<double>((std::numeric_limits<float>::max)()) },
				{ "TabBarBorderSize", &ImGuiStyle::TabBarBorderSize },
				{ "TableAngledHeadersAngle", &ImGuiStyle::TableAngledHeadersAngle,
					-50.0, 50.0 },
				{ "SeparatorTextBorderSize", &ImGuiStyle::SeparatorTextBorderSize },
				{ "MouseCursorScale", &ImGuiStyle::MouseCursorScale, 0.01, 100.0 },
				{ "CurveTessellationTol", &ImGuiStyle::CurveTessellationTol,
					0.01, maximumStyleMagnitude },
				{ "CircleTessellationMaxError", &ImGuiStyle::CircleTessellationMaxError,
					0.01, maximumStyleMagnitude }
			};

			struct VectorField final
			{
				std::string_view Name;
				ImVec2 ImGuiStyle::* Value;
				double Minimum{ -maximumStyleMagnitude };
				double Maximum{ maximumStyleMagnitude };
			};
			static constexpr VectorField vectorFields[]{
				{ "WindowPadding", &ImGuiStyle::WindowPadding },
				{ "WindowMinSize", &ImGuiStyle::WindowMinSize, 1.0 },
				{ "WindowTitleAlign", &ImGuiStyle::WindowTitleAlign },
				{ "FramePadding", &ImGuiStyle::FramePadding },
				{ "ItemSpacing", &ImGuiStyle::ItemSpacing },
				{ "ItemInnerSpacing", &ImGuiStyle::ItemInnerSpacing },
				{ "CellPadding", &ImGuiStyle::CellPadding },
				{ "TouchExtraPadding", &ImGuiStyle::TouchExtraPadding },
				{ "TableAngledHeadersTextAlign",
					&ImGuiStyle::TableAngledHeadersTextAlign },
				{ "ButtonTextAlign", &ImGuiStyle::ButtonTextAlign },
				{ "SelectableTextAlign", &ImGuiStyle::SelectableTextAlign },
				{ "SeparatorTextAlign", &ImGuiStyle::SeparatorTextAlign },
				{ "SeparatorTextPadding", &ImGuiStyle::SeparatorTextPadding },
				{ "DisplayWindowPadding", &ImGuiStyle::DisplayWindowPadding },
				{ "DisplaySafeAreaPadding", &ImGuiStyle::DisplaySafeAreaPadding }
			};

			struct BoolField final
			{
				std::string_view Name;
				bool ImGuiStyle::* Value;
			};
			static constexpr BoolField boolFields[]{
				{ "AntiAliasedLines", &ImGuiStyle::AntiAliasedLines },
				{ "AntiAliasedLinesUseTex", &ImGuiStyle::AntiAliasedLinesUseTex },
				{ "AntiAliasedFill", &ImGuiStyle::AntiAliasedFill }
			};

			for (const auto& field : floatFields) {
				if (!ReadFloat(
						a_json,
						field.Name,
						a_style.*field.Value,
						field.Minimum,
						field.Maximum)) {
					return false;
				}
			}
			for (const auto& field : vectorFields) {
				if (!ReadVector(
						a_json,
						field.Name,
						a_style.*field.Value,
						field.Minimum,
						field.Maximum)) {
					return false;
				}
			}
			for (const auto& field : boolFields) {
				if (!ReadBool(a_json, field.Name, a_style.*field.Value)) {
					return false;
				}
			}
			return ApplyColors(a_json, a_style);
		}

		[[nodiscard]] bool LoadTheme(
			const ThemeEntry& a_theme,
			ImGuiStyle&       a_style)
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

			BuildBaselineStyle(a_style);
			if (!ApplyJsonFields(json, a_style)) {
				logger::warn("Theme '{}' contains an invalid style value", a_theme.Name);
				return false;
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
			if (!LoadTheme(state.Themes[a_index], selection.BaseStyle)) {
				return false;
			}
			selection.Index = a_index;
			selection.UIScale = state.Active.UIScale;
			ApplySelection(selection);
			state.Active = std::move(selection);
			return true;
		}

		[[nodiscard]] bool TryFallbackTheme()
		{
			for (const auto fallback : { "SKYRIMDEFAULT", "CLASSIC" }) {
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
		if (!LoadTheme(state.Themes[a_index], selection.BaseStyle) ||
			!FrameworkSettings::SetMenuStyle(state.Themes[a_index].Name)) {
			return false;
		}
		selection.Index = a_index;
		selection.UIScale =
			state.Pending ? state.Pending->UIScale : state.Active.UIScale;
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
}
