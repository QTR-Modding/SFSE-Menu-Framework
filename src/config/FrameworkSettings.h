#pragma once

#include "audio/SoundTypes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace SFSEMenuFramework::FrameworkSettings
{
	enum class ToggleMode : std::uint8_t
	{
		SinglePress,
		Hold,
		DoublePress,
		Off
	};

	enum class FontRendering : std::uint8_t
	{
		Native,
		Light,
		Auto
	};

	struct Binding final
	{
		std::string_view Name;
		std::uint32_t   Code;
	};

	using MenuStyleName = std::array<char, 64>;
	using FontFileName = std::array<char, 64>;

	struct GlyphCoverage final
	{
		bool Greek{};
		bool Cyrillic{};
		bool Vietnamese{};
		bool Turkish{};
		bool Polish{};
		bool Thai{};
		bool Korean{};
		bool Japanese{};
		bool ChineseSimplifiedCommon{};
		bool ChineseFull{};

		[[nodiscard]] friend constexpr bool operator==(
			const GlyphCoverage&, const GlyphCoverage&) noexcept = default;
	};

	struct FontSettings final
	{
		FontFileName PrimaryFont{};
		float        FontWeight{};
		float        FontSizeMedium{};
		float        MinFontSize{};
		float        MaxFontSize{};
		float        UIScale{};
		FontRendering Rendering{};
		GlyphCoverage Glyphs{};
	};

	struct SettingsSnapshot final
	{
		std::uint32_t ToggleKey{};
		ToggleMode    Mode{};
		std::uint32_t ToggleKeyGamePad{};
		ToggleMode    ModeGamePad{};
		bool          PlayStationIcons{};
		bool          FreezeTimeOnMenu{};
		bool          BlurBackgroundOnMenu{};
		float         BackgroundOpacity{};
		float         WallpaperOpacity{};
		float         WallpaperDimming{};
		MenuStyleName MenuStyle{};
		FontSettings  Fonts{};
		MenuStyleName CursorName{};
		float CursorScale{ 1.0F };
		Audio::Settings Sounds{};
	};

	// Viewport-relative geometry, kept separate from editable framework settings.
	struct WindowLayout final
	{
		const wchar_t* Section{};
		float X{};
		float Y{};
		float Width{};
		float Height{};
	};

	[[nodiscard]] bool LoadWindowLayout(WindowLayout& a_layout) noexcept;
	[[nodiscard]] bool SaveWindowLayouts(std::span<const WindowLayout> a_layouts) noexcept;

	[[nodiscard]] bool Load() noexcept;
	[[nodiscard]] bool Save() noexcept;
	void               ResetDefaults() noexcept;

	[[nodiscard]] std::span<const Binding> GetKeyboardBindings() noexcept;
	[[nodiscard]] std::span<const Binding> GetGamePadBindings() noexcept;
	[[nodiscard]] std::string_view GetKeyboardBindingName(std::uint32_t a_key) noexcept;
	[[nodiscard]] std::string_view GetGamePadBindingName(std::uint32_t a_key) noexcept;

	[[nodiscard]] std::uint32_t GetToggleKey() noexcept;
	[[nodiscard]] ToggleMode    GetToggleMode() noexcept;
	[[nodiscard]] std::uint32_t GetToggleKeyGamePad() noexcept;
	[[nodiscard]] ToggleMode    GetToggleModeGamePad() noexcept;
	[[nodiscard]] bool          GetPlayStationIcons() noexcept;
	[[nodiscard]] bool          GetFreezeTimeOnMenu() noexcept;
	[[nodiscard]] bool          GetBlurBackgroundOnMenu() noexcept;
	[[nodiscard]] float         GetBackgroundOpacity() noexcept;
	[[nodiscard]] float GetWallpaperOpacity() noexcept;
	[[nodiscard]] float GetWallpaperDimming() noexcept;
	[[nodiscard]] MenuStyleName GetMenuStyle() noexcept;
	[[nodiscard]] MenuStyleName GetCursorName() noexcept;
	[[nodiscard]] float GetCursorScale() noexcept;
	[[nodiscard]] Audio::Settings GetSoundSettings() noexcept;
	[[nodiscard]] bool IsValidSoundFileName(std::string_view) noexcept;
	[[nodiscard]] bool SetCursorName(std::string_view) noexcept;
	[[nodiscard]] bool SetCursorScale(float) noexcept;
	[[nodiscard]] FontSettings  GetFontSettings() noexcept;
	[[nodiscard]] FontSettings  GetDefaultFontSettings() noexcept;
	[[nodiscard]] std::string_view GetFontRenderingName(FontRendering a_rendering) noexcept;
	[[nodiscard]] SettingsSnapshot CaptureSnapshot() noexcept;
	void RestoreSnapshot(const SettingsSnapshot& a_snapshot) noexcept;
	[[nodiscard]] std::string_view GetFontFileNameView(const FontFileName& a_name) noexcept;
	[[nodiscard]] bool EqualsIgnoreCaseAscii(
		std::string_view a_left, std::string_view a_right) noexcept;
	[[nodiscard]] bool CopyFontFileName(
		std::string_view a_name, FontFileName& a_result, bool a_validate = true) noexcept;
	[[nodiscard]] bool NormalizeMenuStyleName(
		std::string_view a_name, MenuStyleName& a_result) noexcept;
	[[nodiscard]] bool NormalizeMenuStyleName(
		std::wstring_view a_name, std::string& a_result);
	[[nodiscard]] std::filesystem::path BuildGamePath(std::wstring_view a_relativePath);
	[[nodiscard]] bool FontSettingsEqual(
		const FontSettings& a_left, const FontSettings& a_right,
		float a_tolerance = 0.0F, bool a_ignoreNameCase = false) noexcept;
	[[nodiscard]] bool ValidateFontSettings(const FontSettings& a_settings) noexcept;

	[[nodiscard]] bool SetBackgroundOpacity(float a_opacity) noexcept;
	[[nodiscard]] bool SetWallpaperOpacity(float) noexcept;
	[[nodiscard]] bool SetWallpaperDimming(float) noexcept;
	[[nodiscard]] bool SetMenuStyle(std::string_view a_name) noexcept;
	[[nodiscard]] bool SetFontSettings(const FontSettings& a_settings) noexcept;
}
