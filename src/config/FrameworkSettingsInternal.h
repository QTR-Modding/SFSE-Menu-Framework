#pragma once

#include "config/FrameworkSettings.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace SFSEMenuFramework::FrameworkSettings::Detail
{
	// The ordered key tables, option names, toggle-mode values, defaults,
	// and symbolic INI behavior are adapted from SKSE Menu Framework 3 at
	// commit 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
	// The Win32 profile storage and Starfield input implementation are
	// original to this port.
	inline constexpr std::array keyboardBindings{
		Binding{ "NONE", 0x00 }, Binding{ "ESCAPE", 0x01 },
		Binding{ "1", 0x02 }, Binding{ "2", 0x03 }, Binding{ "3", 0x04 }, Binding{ "4", 0x05 },
		Binding{ "5", 0x06 }, Binding{ "6", 0x07 }, Binding{ "7", 0x08 }, Binding{ "8", 0x09 },
		Binding{ "9", 0x0A }, Binding{ "0", 0x0B }, Binding{ "MINUS", 0x0C }, Binding{ "EQUALS", 0x0D },
		Binding{ "BACKSPACE", 0x0E }, Binding{ "TAB", 0x0F },
		Binding{ "Q", 0x10 }, Binding{ "W", 0x11 }, Binding{ "E", 0x12 }, Binding{ "R", 0x13 },
		Binding{ "T", 0x14 }, Binding{ "Y", 0x15 }, Binding{ "U", 0x16 }, Binding{ "I", 0x17 },
		Binding{ "O", 0x18 }, Binding{ "P", 0x19 }, Binding{ "BRACKETLEFT", 0x1A }, Binding{ "BRACKETRIGHT", 0x1B },
		Binding{ "ENTER", 0x1C }, Binding{ "LEFTCONTROL", 0x1D },
		Binding{ "A", 0x1E }, Binding{ "S", 0x1F }, Binding{ "D", 0x20 }, Binding{ "F", 0x21 },
		Binding{ "G", 0x22 }, Binding{ "H", 0x23 }, Binding{ "J", 0x24 }, Binding{ "K", 0x25 },
		Binding{ "L", 0x26 }, Binding{ "SEMICOLON", 0x27 }, Binding{ "APOSTROPHE", 0x28 }, Binding{ "TILDE", 0x29 },
		Binding{ "LEFTSHIFT", 0x2A }, Binding{ "BACKSLASH", 0x2B },
		Binding{ "Z", 0x2C }, Binding{ "X", 0x2D }, Binding{ "C", 0x2E }, Binding{ "V", 0x2F },
		Binding{ "B", 0x30 }, Binding{ "N", 0x31 }, Binding{ "M", 0x32 }, Binding{ "COMMA", 0x33 },
		Binding{ "PERIOD", 0x34 }, Binding{ "SLASH", 0x35 }, Binding{ "RIGHTSHIFT", 0x36 }, Binding{ "KP_MULTIPLY", 0x37 },
		Binding{ "LEFTALT", 0x38 }, Binding{ "SPACEBAR", 0x39 }, Binding{ "CAPSLOCK", 0x3A },
		Binding{ "F1", 0x3B }, Binding{ "F2", 0x3C }, Binding{ "F3", 0x3D }, Binding{ "F4", 0x3E },
		Binding{ "F5", 0x3F }, Binding{ "F6", 0x40 }, Binding{ "F7", 0x41 }, Binding{ "F8", 0x42 },
		Binding{ "F9", 0x43 }, Binding{ "F10", 0x44 }, Binding{ "NUMLOCK", 0x45 }, Binding{ "SCROLLLOCK", 0x46 },
		Binding{ "KP_7", 0x47 }, Binding{ "KP_8", 0x48 }, Binding{ "KP_9", 0x49 }, Binding{ "KP_SUBTRACT", 0x4A },
		Binding{ "KP_4", 0x4B }, Binding{ "KP_5", 0x4C }, Binding{ "KP_6", 0x4D }, Binding{ "KP_PLUS", 0x4E },
		Binding{ "KP_1", 0x4F }, Binding{ "KP_2", 0x50 }, Binding{ "KP_3", 0x51 }, Binding{ "KP_0", 0x52 },
		Binding{ "KP_DECIMAL", 0x53 }, Binding{ "F11", 0x57 }, Binding{ "F12", 0x58 }, Binding{ "KP_ENTER", 0x9C },
		Binding{ "RIGHTCONTROL", 0x9D }, Binding{ "KP_DIVIDE", 0xB5 }, Binding{ "PRINTSCREEN", 0xB7 },
		Binding{ "RIGHTALT", 0xB8 }, Binding{ "PAUSE", 0xC5 }, Binding{ "HOME", 0xC7 }, Binding{ "UP", 0xC8 },
		Binding{ "PAGEUP", 0xC9 }, Binding{ "LEFT", 0xCB }, Binding{ "RIGHT", 0xCD }, Binding{ "END", 0xCF },
		Binding{ "DOWN", 0xD0 }, Binding{ "PAGEDOWN", 0xD1 }, Binding{ "INSERT", 0xD2 }, Binding{ "DELETE", 0xD3 },
		Binding{ "LEFTWIN", 0xDB }, Binding{ "RIGHTWIN", 0xDC }
	};

	inline constexpr std::array gamePadBindings{
		Binding{ "NONE", 0 }, Binding{ "DPAD_UP", 1 }, Binding{ "DPAD_DOWN", 2 }, Binding{ "DPAD_LEFT", 4 },
		Binding{ "DPAD_RIGHT", 8 }, Binding{ "START", 16 }, Binding{ "BACK", 32 }, Binding{ "LS", 64 },
		Binding{ "RS", 128 }, Binding{ "LB", 256 }, Binding{ "RB", 512 }, Binding{ "LT", 9 },
		Binding{ "RT", 10 }, Binding{ "A", 4096 }, Binding{ "B", 8192 }, Binding{ "X", 16384 }, Binding{ "Y", 32768 }
	};

	inline constexpr float hardMinFontSize = 8.0F;
	inline constexpr float hardMaxFontSize = 96.0F;
	inline constexpr float hardMinUIScale = 0.75F;
	inline constexpr float hardMaxUIScale = 2.0F;
	inline constexpr float hardMinFontWeight = 1.0F;
	inline constexpr float hardMaxFontWeight = 1000.0F;
	inline constexpr float maximumRasterSize = 96.0F;
	inline constexpr float minimumBackgroundOpacity = 0.0F;
	inline constexpr float maximumBackgroundOpacity = 1.0F;
	struct NamedToggleMode final
	{
		const wchar_t* Name;
		ToggleMode     Value;
	};
	inline constexpr std::array toggleModes{
		NamedToggleMode{ L"SINGLEPRESS", ToggleMode::SinglePress },
		NamedToggleMode{ L"HOLD", ToggleMode::Hold },
		NamedToggleMode{ L"DOUBLEPRESS", ToggleMode::DoublePress },
		NamedToggleMode{ L"OFF", ToggleMode::Off }
	};
	struct NamedFontRendering final
	{
		std::string_view Name;
		FontRendering    Value;
	};
	inline constexpr std::array fontRenderingModes{
		NamedFontRendering{ "NATIVE", FontRendering::Native },
		NamedFontRendering{ "LIGHT", FontRendering::Light },
		NamedFontRendering{ "AUTO", FontRendering::Auto }
	};
	template <class Name>
	[[nodiscard]] constexpr Name MakeName(std::string_view a_name) noexcept
	{
		Name result{};
		for (std::size_t index = 0;
		     index < a_name.size() && index + 1 < result.size();
		     ++index) {
			result[index] = a_name[index];
		}
		return result;
	}

	using Values = SettingsSnapshot;
	inline constexpr Values defaultValues{
		.ToggleKey = 0x3B,
		.Mode = ToggleMode::SinglePress,
		.ToggleKeyGamePad = 16,
		.ModeGamePad = ToggleMode::DoublePress,
		.FreezeTimeOnMenu = true,
		.BlurBackgroundOnMenu = true,
		.BackgroundOpacity = 1.0F,
		.WallpaperOpacity = 1.0F,
		.WallpaperDimming = 0.25F,
		.MenuStyle = MakeName<MenuStyleName>("STARFIELD"),
		.Fonts = {
			MakeName<FontFileName>("SpaceGrotesk[wght].ttf"),
			300.0F, 40.0F, 12.0F, 64.0F, 1.0F, FontRendering::Auto, {} },
		.CursorName = MakeName<MenuStyleName>("DEFAULT"),
		.CursorScale = 1.0F
	};
	inline constexpr const auto& defaultFontSettings = defaultValues.Fonts;

	struct UnitSetting final
	{
		const wchar_t* Name;
		float Values::* Member;
	};
	inline constexpr UnitSetting backgroundSettings[]{
		{ L"BackgroundOpacity", &Values::BackgroundOpacity },
		{ L"WallpaperOpacity", &Values::WallpaperOpacity },
		{ L"WallpaperDimming", &Values::WallpaperDimming }
	};

	template <class Character>
	[[nodiscard]] std::uint32_t FoldAscii(Character a_character) noexcept
	{
		const auto value = static_cast<std::uint32_t>(a_character);
		return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
	}

	template <class Left, class Right>
	[[nodiscard]] bool EqualsIgnoreCase(const Left& a_left, const Right& a_right) noexcept
	{
		const std::basic_string_view left{ a_left };
		const std::basic_string_view right{ a_right };
		if (left.size() != right.size()) {
			return false;
		}
		for (std::size_t index = 0; index < left.size(); ++index) {
			if (FoldAscii(left[index]) != FoldAscii(right[index])) {
				return false;
			}
		}
		return true;
	}
	template <std::size_t N>
	[[nodiscard]] const Binding* FindBinding(
		const std::array<Binding, N>& a_bindings, std::uint32_t a_code) noexcept
	{
		for (const auto& binding : a_bindings) {
			if (binding.Code == a_code) {
				return &binding;
			}
		}
		return nullptr;
	}

	template <std::size_t N>
	[[nodiscard]] std::string_view BindingName(
		const std::array<Binding, N>& a_bindings, std::uint32_t a_code) noexcept
	{
		const auto* binding = FindBinding(a_bindings, a_code);
		return binding ? binding->Name : std::string_view{};
	}
	[[nodiscard]] inline bool IsValidLeafName(
		std::string_view a_name, std::size_t a_capacity) noexcept
	{
		if (a_name.empty() || a_name.size() >= a_capacity ||
			a_name == "." || a_name == ".." || a_name.front() == ' ' ||
			a_name.back() == '.' || a_name.back() == ' ') {
			return false;
		}

		for (const auto character : a_name) {
			const auto value = static_cast<unsigned char>(character);
			if (value < 0x20 || value > 0x7E || character == '<' ||
				character == '>' || character == ':' || character == '"' ||
				character == '/' || character == '\\' || character == '|' ||
				character == '?' || character == '*') {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] inline bool IsValidMenuStyleName(
		std::string_view a_name) noexcept
	{
		return IsValidLeafName(a_name, MenuStyleName{}.size());
	}

	[[nodiscard]] inline bool IsValidFontFileName(
		std::string_view a_name) noexcept
	{
		if (!IsValidLeafName(a_name, FontFileName{}.size()) ||
			a_name.size() < 5) {
			return false;
		}
		const auto extension = a_name.substr(a_name.size() - 4);
		return EqualsIgnoreCase(extension, ".ttf") ||
		       EqualsIgnoreCase(extension, ".otf");
	}

	template <class Name>
	[[nodiscard]] std::string_view NameView(const Name& a_name) noexcept
	{
		const auto end = std::ranges::find(a_name, '\0');
		return end == a_name.end() ?
			std::string_view{} :
			std::string_view{ a_name.data(), static_cast<std::size_t>(end - a_name.begin()) };
	}

	template <class Name>
	[[nodiscard]] bool CopyName(
		std::string_view a_name,
		Name&            a_result,
		bool             a_uppercase,
		bool             a_fontName) noexcept
	{
		if (!(a_fontName ? IsValidFontFileName(a_name) :
			IsValidMenuStyleName(a_name))) {
			return false;
		}
		a_result.fill('\0');
		for (std::size_t index = 0; index < a_name.size(); ++index) {
			a_result[index] = static_cast<char>(
				a_uppercase ? FoldAscii(a_name[index]) :
				static_cast<unsigned char>(a_name[index]));
		}
		return true;
	}
	struct FloatSetting final
	{
		const wchar_t* Name;
		float FontSettings::* Value;
		float Default;
		float Minimum;
		float Maximum;
	};
	inline constexpr std::array fontFloatSettings{
		FloatSetting{
			L"FontWeight", &FontSettings::FontWeight,
			defaultFontSettings.FontWeight, hardMinFontWeight, hardMaxFontWeight },
		FloatSetting{
			L"FontSizeMedium", &FontSettings::FontSizeMedium,
			defaultFontSettings.FontSizeMedium, hardMinFontSize, hardMaxFontSize },
		FloatSetting{
			L"MinFontSize", &FontSettings::MinFontSize,
			defaultFontSettings.MinFontSize, hardMinFontSize, hardMaxFontSize },
		FloatSetting{
			L"MaxFontSize", &FontSettings::MaxFontSize,
			defaultFontSettings.MaxFontSize, hardMinFontSize, hardMaxFontSize },
		FloatSetting{
			L"UIScale", &FontSettings::UIScale,
			defaultFontSettings.UIScale, hardMinUIScale, hardMaxUIScale }
	};
	struct GlyphSetting final
	{
		const wchar_t* Name;
		bool GlyphCoverage::* Value;
	};
	// The optional language-range toggles and shipped false defaults adapt
	// SKSE Menu Framework 3 Config.cpp/FontManager.cpp at commit
	// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
	// Polish follows the merged source; Greek, Vietnamese, and the two Chinese
	// choices extend that configuration.
	inline constexpr std::array glyphSettings{
		GlyphSetting{ L"EnableGreek", &GlyphCoverage::Greek },
		GlyphSetting{ L"EnableCyrillic", &GlyphCoverage::Cyrillic },
		GlyphSetting{ L"EnableVietnamese", &GlyphCoverage::Vietnamese },
		GlyphSetting{ L"EnableTurkish", &GlyphCoverage::Turkish },
		GlyphSetting{ L"EnablePolish", &GlyphCoverage::Polish },
		GlyphSetting{ L"EnableThai", &GlyphCoverage::Thai },
		GlyphSetting{ L"EnableKorean", &GlyphCoverage::Korean },
		GlyphSetting{ L"EnableJapanese", &GlyphCoverage::Japanese },
		GlyphSetting{
			L"EnableChineseSimplifiedCommon",
			&GlyphCoverage::ChineseSimplifiedCommon },
		GlyphSetting{ L"EnableChineseFull", &GlyphCoverage::ChineseFull }
	};

	[[nodiscard]] bool NormalizeFontSettings(
		FontSettings& a_settings) noexcept;
	[[nodiscard]] const wchar_t* ToggleModeName(
		ToggleMode a_mode) noexcept;
	[[nodiscard]] Values GetValues() noexcept;
	void SetValues(const Values& a_values) noexcept;
	void Normalize(Values& a_values) noexcept;
}
