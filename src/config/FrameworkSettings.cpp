#include "config/FrameworkSettings.h"
#include "config/FrameworkSettingsInternal.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace SFSEMenuFramework::FrameworkSettings
{
	using namespace Detail;

	namespace
	{
		SRWLOCK stateLock = SRWLOCK_INIT;

		class StateLockGuard final
		{
		public:
			StateLockGuard() noexcept { ::AcquireSRWLockExclusive(&stateLock); }
			~StateLockGuard() { ::ReleaseSRWLockExclusive(&stateLock); }
			StateLockGuard(const StateLockGuard&) = delete;
			StateLockGuard& operator=(const StateLockGuard&) = delete;
		};

		Values           values{ defaultValues };
		[[nodiscard]] bool CopyMenuStyleName(
			std::string_view a_name, MenuStyleName& a_result) noexcept
		{
			return CopyName(a_name, a_result, true, false);
		}

		[[nodiscard]] bool IsValidFontSettings(
			const FontSettings& a_settings) noexcept
		{
			const auto primaryFont = NameView(a_settings.PrimaryFont);
			return IsValidFontFileName(primaryFont) &&
			       std::isfinite(a_settings.FontWeight) &&
			       std::isfinite(a_settings.FontSizeMedium) &&
			       std::isfinite(a_settings.MinFontSize) &&
			       std::isfinite(a_settings.MaxFontSize) &&
			       std::isfinite(a_settings.UIScale) &&
			       a_settings.FontWeight >= hardMinFontWeight &&
			       a_settings.FontWeight <= hardMaxFontWeight &&
			       a_settings.MinFontSize >= hardMinFontSize &&
			       a_settings.MaxFontSize <= hardMaxFontSize &&
			       a_settings.MinFontSize <= a_settings.MaxFontSize &&
			       a_settings.FontSizeMedium >= a_settings.MinFontSize &&
			       a_settings.FontSizeMedium <= a_settings.MaxFontSize &&
			       std::to_underlying(a_settings.Rendering) <=
				       std::to_underlying(FontRendering::Auto) &&
			       a_settings.UIScale >= hardMinUIScale &&
			       a_settings.UIScale <= hardMaxUIScale &&
			       a_settings.FontSizeMedium * a_settings.UIScale <=
				       maximumRasterSize &&
			       !(a_settings.Glyphs.ChineseSimplifiedCommon &&
			         a_settings.Glyphs.ChineseFull);
		}

	}

	bool Detail::NormalizeFontSettings(
		FontSettings& a_settings) noexcept
	{
		bool unchanged = true;
		FontFileName normalizedName{};
		if (!CopyFontFileName(
				NameView(a_settings.PrimaryFont),
				normalizedName)) {
			normalizedName = defaultFontSettings.PrimaryFont;
			unchanged = false;
		} else if (normalizedName != a_settings.PrimaryFont) {
			unchanged = false;
		}
		a_settings.PrimaryFont = normalizedName;
		if (a_settings.Glyphs.ChineseSimplifiedCommon &&
			a_settings.Glyphs.ChineseFull) {
			a_settings.Glyphs.ChineseSimplifiedCommon = false;
			unchanged = false;
		}
		if (std::to_underlying(a_settings.Rendering) >
			std::to_underlying(FontRendering::Auto)) {
			a_settings.Rendering = defaultFontSettings.Rendering;
			unchanged = false;
		}

		auto normalizeScalar = [&unchanged](
			float& a_value,
			float  a_default,
			float  a_minimum,
			float  a_maximum) {
			const auto normalized = std::isfinite(a_value) ?
				std::clamp(a_value, a_minimum, a_maximum) :
				a_default;
			if (normalized != a_value) {
				unchanged = false;
				a_value = normalized;
			}
		};

		for (const auto& setting : fontFloatSettings) {
			if (setting.Value != &FontSettings::FontSizeMedium) {
				normalizeScalar(
					a_settings.*setting.Value, setting.Default,
					setting.Minimum, setting.Maximum);
			}
		}
		if (a_settings.MinFontSize > a_settings.MaxFontSize) {
			std::swap(a_settings.MinFontSize, a_settings.MaxFontSize);
			unchanged = false;
		}
		normalizeScalar(
			a_settings.FontSizeMedium,
			defaultFontSettings.FontSizeMedium,
			a_settings.MinFontSize,
			a_settings.MaxFontSize);
		if (a_settings.FontSizeMedium * a_settings.UIScale >
			maximumRasterSize) {
			const auto largestFont = maximumRasterSize / a_settings.UIScale;
			if (largestFont >= a_settings.MinFontSize) {
				a_settings.FontSizeMedium = largestFont;
			} else {
				a_settings.FontSizeMedium = a_settings.MinFontSize;
				a_settings.UIScale =
					maximumRasterSize / a_settings.MinFontSize;
			}
			unchanged = false;
		}
		return unchanged;
	}

	const wchar_t* Detail::ToggleModeName(ToggleMode a_mode) noexcept
	{
		for (const auto& mode : toggleModes) {
			if (mode.Value == a_mode) {
				return mode.Name;
			}
		}
		return toggleModes.front().Name;
	}

	Detail::Values Detail::GetValues() noexcept
	{
		const StateLockGuard lock;
		return values;
	}

	void Detail::SetValues(const Values& a_values) noexcept
	{
		const StateLockGuard lock;
		values = a_values;
	}

	void Detail::Normalize(Values& a_values) noexcept
	{
		if (!FindBinding(keyboardBindings, a_values.ToggleKey)) {
			a_values.ToggleKey = defaultValues.ToggleKey;
		}
		if (std::to_underlying(a_values.Mode) > std::to_underlying(ToggleMode::Off)) {
			a_values.Mode = defaultValues.Mode;
		}
		if (!FindBinding(gamePadBindings, a_values.ToggleKeyGamePad)) {
			a_values.ToggleKeyGamePad = defaultValues.ToggleKeyGamePad;
		}
		if (std::to_underlying(a_values.ModeGamePad) > std::to_underlying(ToggleMode::Off)) {
			a_values.ModeGamePad = defaultValues.ModeGamePad;
		}
		if (!IsValidMenuStyleName(NameView(a_values.MenuStyle))) {
			a_values.MenuStyle = defaultValues.MenuStyle;
		}
		static_cast<void>(NormalizeFontSettings(a_values.Fonts));
	}

	namespace
	{
		template <class Value>
		void WriteMember(Value Values::* a_member, const Value& a_value) noexcept
		{
			const StateLockGuard lock;
			values.*a_member = a_value;
		}
	}

	std::span<const Binding> GetKeyboardBindings() noexcept { return keyboardBindings; }
	std::span<const Binding> GetGamePadBindings() noexcept { return gamePadBindings; }
	std::string_view GetKeyboardBindingName(std::uint32_t a_key) noexcept
	{ return BindingName(keyboardBindings, a_key); }
	std::string_view GetGamePadBindingName(std::uint32_t a_key) noexcept
	{ return BindingName(gamePadBindings, a_key); }
	std::string_view GetFontRenderingName(FontRendering a_rendering) noexcept
	{
		for (const auto& rendering : fontRenderingModes) {
			if (rendering.Value == a_rendering) {
				return rendering.Name;
			}
		}
		return fontRenderingModes.front().Name;
	}
	void ResetDefaults() noexcept { SetValues(defaultValues); }

	std::uint32_t GetToggleKey() noexcept { return GetValues().ToggleKey; }
	ToggleMode GetToggleMode() noexcept { return GetValues().Mode; }
	std::uint32_t GetToggleKeyGamePad() noexcept { return GetValues().ToggleKeyGamePad; }
	ToggleMode GetToggleModeGamePad() noexcept { return GetValues().ModeGamePad; }
	bool GetFreezeTimeOnMenu() noexcept { return GetValues().FreezeTimeOnMenu; }
	bool GetBlurBackgroundOnMenu() noexcept { return GetValues().BlurBackgroundOnMenu; }
	MenuStyleName GetMenuStyle() noexcept { return GetValues().MenuStyle; }
	FontSettings GetFontSettings() noexcept { return GetValues().Fonts; }
	FontSettings GetDefaultFontSettings() noexcept { return defaultFontSettings; }
	SettingsSnapshot CaptureSnapshot() noexcept { return GetValues(); }
	void RestoreSnapshot(const SettingsSnapshot& a_snapshot) noexcept
	{
		auto restored = a_snapshot;
		Normalize(restored);
		SetValues(restored);
	}
	std::string_view GetFontFileNameView(const FontFileName& a_name) noexcept { return NameView(a_name); }

	bool EqualsIgnoreCaseAscii(
		std::string_view a_left,
		std::string_view a_right) noexcept
	{ return EqualsIgnoreCase(a_left, a_right); }

	bool CopyFontFileName(
		std::string_view a_name,
		FontFileName&    a_result,
		bool             a_validate) noexcept
	{
		if (a_validate) {
			return CopyName(a_name, a_result, false, true);
		}
		if (a_name.empty() || a_name.size() >= a_result.size()) {
			return false;
		}
		a_result.fill('\0');
		std::ranges::copy(a_name, a_result.begin());
		return true;
	}

	bool NormalizeMenuStyleName(
		std::string_view a_name,
		MenuStyleName&   a_result) noexcept
	{
		return CopyMenuStyleName(a_name, a_result);
	}

	bool NormalizeMenuStyleName(
		std::wstring_view a_name,
		std::string&      a_result)
	{
		std::string ascii;
		ascii.reserve(a_name.size());
		for (const auto character : a_name) {
			if (static_cast<std::uint32_t>(character) > 0x7F) {
				return false;
			}
			ascii.push_back(static_cast<char>(character));
		}
		MenuStyleName normalized{};
		if (!CopyMenuStyleName(ascii, normalized)) {
			return false;
		}
		a_result = NameView(normalized);
		return true;
	}
	bool FontSettingsEqual(
		const FontSettings& a_left,
		const FontSettings& a_right,
		float               a_tolerance,
		bool                a_ignoreNameCase) noexcept
	{
		const auto close = [a_tolerance](float a_first, float a_second) {
			return std::fabs(a_first - a_second) <= a_tolerance;
		};
		const bool sameFont = a_ignoreNameCase ?
			EqualsIgnoreCase(
				NameView(a_left.PrimaryFont), NameView(a_right.PrimaryFont)) :
			a_left.PrimaryFont == a_right.PrimaryFont;
		return sameFont &&
		       a_left.Rendering == a_right.Rendering &&
		       a_left.Glyphs == a_right.Glyphs &&
		       close(a_left.FontWeight, a_right.FontWeight) &&
		       close(a_left.FontSizeMedium, a_right.FontSizeMedium) &&
		       close(a_left.MinFontSize, a_right.MinFontSize) &&
		       close(a_left.MaxFontSize, a_right.MaxFontSize) &&
		       close(a_left.UIScale, a_right.UIScale);
	}

	bool ValidateFontSettings(const FontSettings& a_settings) noexcept { return IsValidFontSettings(a_settings); }

	bool SetMenuStyle(std::string_view a_name) noexcept
	{
		MenuStyleName normalized{};
		if (!CopyMenuStyleName(a_name, normalized)) {
			return false;
		}

		WriteMember(&Values::MenuStyle, normalized);
		return true;
	}

	bool SetFontSettings(const FontSettings& a_settings) noexcept
	{
		if (!IsValidFontSettings(a_settings)) {
			return false;
		}

		FontSettings normalized = a_settings;
		if (!CopyFontFileName(
				NameView(a_settings.PrimaryFont),
				normalized.PrimaryFont)) {
			return false;
		}

		WriteMember(&Values::Fonts, normalized);
		return true;
	}
}
