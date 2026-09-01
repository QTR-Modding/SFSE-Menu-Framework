#include "FrameworkSettings.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace SFSEMenuFramework::FrameworkSettings
{
	namespace
	{
		// The ordered key tables, option names, toggle-mode values, defaults,
		// and symbolic INI behavior are adapted from SKSE Menu Framework 3 at
		// commit 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// The Win32 profile storage and Starfield input implementation are
		// original to this port.
		constexpr std::array keyboardBindings{
			Binding{ "NONE", 0x00 },
			Binding{ "ESCAPE", 0x01 },
			Binding{ "1", 0x02 },
			Binding{ "2", 0x03 },
			Binding{ "3", 0x04 },
			Binding{ "4", 0x05 },
			Binding{ "5", 0x06 },
			Binding{ "6", 0x07 },
			Binding{ "7", 0x08 },
			Binding{ "8", 0x09 },
			Binding{ "9", 0x0A },
			Binding{ "0", 0x0B },
			Binding{ "MINUS", 0x0C },
			Binding{ "EQUALS", 0x0D },
			Binding{ "BACKSPACE", 0x0E },
			Binding{ "TAB", 0x0F },
			Binding{ "Q", 0x10 },
			Binding{ "W", 0x11 },
			Binding{ "E", 0x12 },
			Binding{ "R", 0x13 },
			Binding{ "T", 0x14 },
			Binding{ "Y", 0x15 },
			Binding{ "U", 0x16 },
			Binding{ "I", 0x17 },
			Binding{ "O", 0x18 },
			Binding{ "P", 0x19 },
			Binding{ "BRACKETLEFT", 0x1A },
			Binding{ "BRACKETRIGHT", 0x1B },
			Binding{ "ENTER", 0x1C },
			Binding{ "LEFTCONTROL", 0x1D },
			Binding{ "A", 0x1E },
			Binding{ "S", 0x1F },
			Binding{ "D", 0x20 },
			Binding{ "F", 0x21 },
			Binding{ "G", 0x22 },
			Binding{ "H", 0x23 },
			Binding{ "J", 0x24 },
			Binding{ "K", 0x25 },
			Binding{ "L", 0x26 },
			Binding{ "SEMICOLON", 0x27 },
			Binding{ "APOSTROPHE", 0x28 },
			Binding{ "TILDE", 0x29 },
			Binding{ "LEFTSHIFT", 0x2A },
			Binding{ "BACKSLASH", 0x2B },
			Binding{ "Z", 0x2C },
			Binding{ "X", 0x2D },
			Binding{ "C", 0x2E },
			Binding{ "V", 0x2F },
			Binding{ "B", 0x30 },
			Binding{ "N", 0x31 },
			Binding{ "M", 0x32 },
			Binding{ "COMMA", 0x33 },
			Binding{ "PERIOD", 0x34 },
			Binding{ "SLASH", 0x35 },
			Binding{ "RIGHTSHIFT", 0x36 },
			Binding{ "KP_MULTIPLY", 0x37 },
			Binding{ "LEFTALT", 0x38 },
			Binding{ "SPACEBAR", 0x39 },
			Binding{ "CAPSLOCK", 0x3A },
			Binding{ "F1", 0x3B },
			Binding{ "F2", 0x3C },
			Binding{ "F3", 0x3D },
			Binding{ "F4", 0x3E },
			Binding{ "F5", 0x3F },
			Binding{ "F6", 0x40 },
			Binding{ "F7", 0x41 },
			Binding{ "F8", 0x42 },
			Binding{ "F9", 0x43 },
			Binding{ "F10", 0x44 },
			Binding{ "NUMLOCK", 0x45 },
			Binding{ "SCROLLLOCK", 0x46 },
			Binding{ "KP_7", 0x47 },
			Binding{ "KP_8", 0x48 },
			Binding{ "KP_9", 0x49 },
			Binding{ "KP_SUBTRACT", 0x4A },
			Binding{ "KP_4", 0x4B },
			Binding{ "KP_5", 0x4C },
			Binding{ "KP_6", 0x4D },
			Binding{ "KP_PLUS", 0x4E },
			Binding{ "KP_1", 0x4F },
			Binding{ "KP_2", 0x50 },
			Binding{ "KP_3", 0x51 },
			Binding{ "KP_0", 0x52 },
			Binding{ "KP_DECIMAL", 0x53 },
			Binding{ "F11", 0x57 },
			Binding{ "F12", 0x58 },
			Binding{ "KP_ENTER", 0x9C },
			Binding{ "RIGHTCONTROL", 0x9D },
			Binding{ "KP_DIVIDE", 0xB5 },
			Binding{ "PRINTSCREEN", 0xB7 },
			Binding{ "RIGHTALT", 0xB8 },
			Binding{ "PAUSE", 0xC5 },
			Binding{ "HOME", 0xC7 },
			Binding{ "UP", 0xC8 },
			Binding{ "PAGEUP", 0xC9 },
			Binding{ "LEFT", 0xCB },
			Binding{ "RIGHT", 0xCD },
			Binding{ "END", 0xCF },
			Binding{ "DOWN", 0xD0 },
			Binding{ "PAGEDOWN", 0xD1 },
			Binding{ "INSERT", 0xD2 },
			Binding{ "DELETE", 0xD3 },
			Binding{ "LEFTWIN", 0xDB },
			Binding{ "RIGHTWIN", 0xDC }
		};

		constexpr std::array gamePadBindings{
			Binding{ "NONE", 0 },
			Binding{ "DPAD_UP", 1 },
			Binding{ "DPAD_DOWN", 2 },
			Binding{ "DPAD_LEFT", 4 },
			Binding{ "DPAD_RIGHT", 8 },
			Binding{ "START", 16 },
			Binding{ "BACK", 32 },
			Binding{ "LS", 64 },
			Binding{ "RS", 128 },
			Binding{ "LB", 256 },
			Binding{ "RB", 512 },
			Binding{ "LT", 9 },
			Binding{ "RT", 10 },
			Binding{ "A", 4096 },
			Binding{ "B", 8192 },
			Binding{ "X", 16384 },
			Binding{ "Y", 32768 }
		};

		constexpr std::uint32_t defaultToggleKey = 0x3B;
		constexpr ToggleMode    defaultToggleMode = ToggleMode::SinglePress;
		constexpr std::uint32_t defaultToggleKeyGamePad = 16;
		constexpr ToggleMode defaultToggleModeGamePad = ToggleMode::DoublePress;
		constexpr bool       defaultFreezeTimeOnMenu = true;
		constexpr bool       defaultBlurBackgroundOnMenu = true;
		constexpr std::string_view defaultMenuStyleName = "CLASSIC";
		constexpr std::string_view defaultPrimaryFontName =
			"Jost-400-Book.ttf";
		constexpr float defaultFontWeight = 500.0F;
		constexpr float defaultFontSizeMedium = 48.0F;
		constexpr float defaultMinFontSize = 12.0F;
		constexpr float defaultMaxFontSize = 64.0F;
		constexpr float defaultUIScale = 1.0F;
		constexpr float hardMinFontSize = 8.0F;
		constexpr float hardMaxFontSize = 96.0F;
		constexpr float hardMinUIScale = 0.75F;
		constexpr float hardMaxUIScale = 2.0F;
		constexpr float hardMinFontWeight = 1.0F;
		constexpr float hardMaxFontWeight = 1000.0F;
		constexpr float maximumRasterSize = 96.0F;
		struct NamedToggleMode final
		{
			const wchar_t* Name;
			ToggleMode     Value;
		};
		constexpr std::array toggleModes{
			NamedToggleMode{ L"SINGLEPRESS", ToggleMode::SinglePress },
			NamedToggleMode{ L"HOLD", ToggleMode::Hold },
			NamedToggleMode{ L"DOUBLEPRESS", ToggleMode::DoublePress },
			NamedToggleMode{ L"OFF", ToggleMode::Off }
		};

		constexpr wchar_t sectionName[]{ L"General" };
		constexpr wchar_t fontSectionName[]{ L"Fonts" };
		constexpr wchar_t relativePath[]{
			L"Data\\SFSE\\Plugins\\SFSEMenuFramework.ini"
		};
		constexpr wchar_t temporarySuffix[]{ L".tmp" };
		constexpr wchar_t missingValueSentinel[]{ L"\x1F" };
		constexpr std::size_t pathCapacity = 32768;
		constexpr std::size_t valueCapacity = FontFileName{}.size() + 1;

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

		constexpr auto defaultMenuStyle =
			MakeName<MenuStyleName>(defaultMenuStyleName);

		constexpr FontSettings defaultFontSettings{
			MakeName<FontFileName>(defaultPrimaryFontName),
			defaultFontWeight,
			defaultFontSizeMedium,
			defaultMinFontSize,
			defaultMaxFontSize,
			defaultUIScale
		};

		using Values = SettingsSnapshot;
		constexpr Values defaultValues{
			defaultToggleKey,
			defaultToggleMode,
			defaultToggleKeyGamePad,
			defaultToggleModeGamePad,
			defaultFreezeTimeOnMenu,
			defaultBlurBackgroundOnMenu,
			defaultMenuStyle,
			defaultFontSettings
		};
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

		[[nodiscard]] bool IsAsciiSpace(wchar_t a_character) noexcept
		{
			return a_character == L' ' || a_character == L'\t' ||
			       a_character == L'\r' || a_character == L'\n';
		}

		[[nodiscard]] std::wstring_view Trim(std::wstring_view a_value) noexcept
		{
			while (!a_value.empty() && IsAsciiSpace(a_value.front())) {
				a_value.remove_prefix(1);
			}
			while (!a_value.empty() && IsAsciiSpace(a_value.back())) {
				a_value.remove_suffix(1);
			}
			return a_value;
		}

		template <class Character>
		[[nodiscard]] std::uint32_t FoldAscii(Character a_character) noexcept
		{
			const auto value = static_cast<std::uint32_t>(a_character);
			return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
		}

		template <class LeftCharacter, class RightCharacter>
		[[nodiscard]] bool EqualsIgnoreCaseBasic(
			std::basic_string_view<LeftCharacter>  a_left,
			std::basic_string_view<RightCharacter> a_right) noexcept
		{
			if (a_left.size() != a_right.size()) {
				return false;
			}
			for (std::size_t index = 0; index < a_left.size(); ++index) {
				if (FoldAscii(a_left[index]) != FoldAscii(a_right[index])) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool EqualsIgnoreCase(
			std::wstring_view a_left,
			std::wstring_view a_right) noexcept
		{
			return EqualsIgnoreCaseBasic(a_left, a_right);
		}

		[[nodiscard]] bool EqualsIgnoreCase(
			std::wstring_view a_left,
			std::string_view  a_right) noexcept
		{
			return EqualsIgnoreCaseBasic(a_left, a_right);
		}

		[[nodiscard]] bool EqualsIgnoreCase(
			std::string_view a_left,
			std::string_view a_right) noexcept
		{
			return EqualsIgnoreCaseBasic(a_left, a_right);
		}

		[[nodiscard]] bool BuildSettingsPath(
			std::array<wchar_t, pathCapacity>& a_path) noexcept
		{
			const auto length = ::GetModuleFileNameW(
				nullptr,
				a_path.data(),
				static_cast<DWORD>(a_path.size()));
			if (length == 0 || length >= a_path.size()) {
				return false;
			}

			std::size_t directoryLength = length;
			while (directoryLength > 0 &&
			       a_path[directoryLength - 1] != L'\\' &&
			       a_path[directoryLength - 1] != L'/') {
				--directoryLength;
			}
			if (directoryLength == 0) {
				return false;
			}

			constexpr auto suffixLength = std::size(relativePath) - 1;
			if (directoryLength + suffixLength >= a_path.size()) {
				return false;
			}
			std::wmemcpy(
				a_path.data() + directoryLength,
				relativePath,
				suffixLength + 1);
			return true;
		}

		[[nodiscard]] bool BuildTemporarySettingsPath(
			const std::array<wchar_t, pathCapacity>& a_path,
			std::array<wchar_t, pathCapacity>&       a_temporaryPath) noexcept
		{
			std::size_t length{};
			while (length < a_path.size() && a_path[length] != L'\0') {
				++length;
			}
			constexpr auto suffixLength = std::size(temporarySuffix) - 1;
			if (length == a_path.size() ||
				length + suffixLength >= a_temporaryPath.size()) {
				return false;
			}

			std::wmemcpy(a_temporaryPath.data(), a_path.data(), length);
			std::wmemcpy(
				a_temporaryPath.data() + length,
				temporarySuffix,
				suffixLength + 1);
			return true;
		}

		[[nodiscard]] bool PrepareTemporarySettingsFile(
			const wchar_t* a_path,
			const wchar_t* a_temporaryPath) noexcept
		{
			const auto attributes = ::GetFileAttributesW(a_path);
			if (attributes != INVALID_FILE_ATTRIBUTES) {
				return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
				       ::CopyFileW(a_path, a_temporaryPath, FALSE) != FALSE;
			}

			const auto error = ::GetLastError();
			if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
				return false;
			}

			const auto file = ::CreateFileW(
				a_temporaryPath,
				GENERIC_WRITE,
				0,
				nullptr,
				CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return false;
			}
			return ::CloseHandle(file) != FALSE;
		}

		template <class Value>
		[[nodiscard]] bool ParseNumber(
			std::wstring_view a_text,
			Value&            a_result) noexcept
		{
			a_text = Trim(a_text);
			if (a_text.empty() || a_text.size() >= valueCapacity) {
				return false;
			}

			std::array<char, valueCapacity> narrow{};
			for (std::size_t index = 0; index < a_text.size(); ++index) {
				if (static_cast<std::uint32_t>(a_text[index]) > 0x7F) {
					return false;
				}
				narrow[index] = static_cast<char>(a_text[index]);
			}

			Value parsed{};
			const auto result = std::from_chars(
				narrow.data(),
				narrow.data() + a_text.size(), parsed);
			if (result.ec != std::errc{} ||
				result.ptr != narrow.data() + a_text.size() ||
				(std::is_floating_point_v<Value> && !std::isfinite(parsed))) {
				return false;
			}
			a_result = parsed;
			return true;
		}

		template <std::size_t N>
		[[nodiscard]] const Binding* FindBinding(
			const std::array<Binding, N>& a_bindings,
			std::uint32_t                 a_code) noexcept
		{
			for (const auto& binding : a_bindings) {
				if (binding.Code == a_code) {
					return &binding;
				}
			}
			return nullptr;
		}

		template <std::size_t N>
		[[nodiscard]] bool ParseBinding(
			std::wstring_view             a_text,
			const std::array<Binding, N>& a_bindings,
			std::uint32_t&                a_result) noexcept
		{
			a_text = Trim(a_text);
			for (const auto& binding : a_bindings) {
				if (EqualsIgnoreCase(a_text, binding.Name)) {
					a_result = binding.Code;
					return true;
				}
			}

			std::uint32_t numeric{};
			if (ParseNumber(a_text, numeric) &&
				FindBinding(a_bindings, numeric)) {
				a_result = numeric;
				return true;
			}
			return false;
		}

		template <std::size_t N>
		[[nodiscard]] std::string_view BindingName(
			const std::array<Binding, N>& a_bindings,
			std::uint32_t                 a_code) noexcept
		{
			const auto* binding = FindBinding(a_bindings, a_code);
			return binding ? binding->Name : std::string_view{};
		}

		[[nodiscard]] bool ParseToggleMode(
			std::wstring_view a_text,
			ToggleMode&       a_result) noexcept
		{
			a_text = Trim(a_text);
			for (const auto& mode : toggleModes) {
				if (EqualsIgnoreCase(a_text, mode.Name)) {
					a_result = mode.Value;
					return true;
				}
			}

			std::uint32_t numeric{};
			if (!ParseNumber(a_text, numeric) ||
				numeric > std::to_underlying(ToggleMode::Off)) {
				return false;
			}
			a_result = static_cast<ToggleMode>(numeric);
			return true;
		}

		[[nodiscard]] bool ParseBool(
			std::wstring_view a_text,
			bool&             a_result) noexcept
		{
			a_text = Trim(a_text);
			if (a_text == L"1" || EqualsIgnoreCase(a_text, L"TRUE") ||
				EqualsIgnoreCase(a_text, L"ON")) {
				a_result = true;
				return true;
			}
			if (a_text == L"0" || EqualsIgnoreCase(a_text, L"FALSE") ||
				EqualsIgnoreCase(a_text, L"OFF")) {
				a_result = false;
				return true;
			}
			return false;
		}

		[[nodiscard]] bool IsValidLeafName(
			std::string_view a_name,
			std::size_t      a_capacity) noexcept
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

		[[nodiscard]] bool IsValidMenuStyleName(std::string_view a_name) noexcept
		{
			return IsValidLeafName(a_name, MenuStyleName{}.size());
		}

		[[nodiscard]] bool IsValidFontFileName(std::string_view a_name) noexcept
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

		template <class Name>
		[[nodiscard]] bool ParseName(
			std::wstring_view a_text,
			Name&             a_result,
			bool              a_uppercase,
			bool              a_fontName) noexcept
		{
			a_text = Trim(a_text);
			if (a_text.empty() || a_text.size() >= a_result.size()) {
				return false;
			}
			Name parsed{};
			for (std::size_t index = 0; index < a_text.size(); ++index) {
				if (static_cast<std::uint32_t>(a_text[index]) > 0x7F) {
					return false;
				}
				parsed[index] = static_cast<char>(a_text[index]);
			}
			return CopyName(NameView(parsed), a_result, a_uppercase, a_fontName);
		}

		[[nodiscard]] bool CopyMenuStyleName(
			std::string_view a_name,
			MenuStyleName&   a_result) noexcept
		{
			return CopyName(a_name, a_result, true, false);
		}

		[[nodiscard]] bool ParseMenuStyleName(
			std::wstring_view a_text,
			MenuStyleName&    a_result) noexcept
		{
			return ParseName(a_text, a_result, true, false);
		}

		[[nodiscard]] bool ParseFontFileName(
			std::wstring_view a_text,
			FontFileName&    a_result) noexcept
		{
			return ParseName(a_text, a_result, false, true);
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
			       a_settings.UIScale >= hardMinUIScale &&
			       a_settings.UIScale <= hardMaxUIScale &&
			       a_settings.FontSizeMedium * a_settings.UIScale <=
				       maximumRasterSize;
		}

		struct FloatSetting final
		{
			const wchar_t* Name;
			float FontSettings::* Value;
			float Default;
			float Minimum;
			float Maximum;
		};
		constexpr std::array fontFloatSettings{
			FloatSetting{ L"FontWeight", &FontSettings::FontWeight, defaultFontWeight, hardMinFontWeight, hardMaxFontWeight },
			FloatSetting{ L"FontSizeMedium", &FontSettings::FontSizeMedium, defaultFontSizeMedium, hardMinFontSize, hardMaxFontSize },
			FloatSetting{ L"MinFontSize", &FontSettings::MinFontSize, defaultMinFontSize, hardMinFontSize, hardMaxFontSize },
			FloatSetting{ L"MaxFontSize", &FontSettings::MaxFontSize, defaultMaxFontSize, hardMinFontSize, hardMaxFontSize },
			FloatSetting{ L"UIScale", &FontSettings::UIScale, defaultUIScale, hardMinUIScale, hardMaxUIScale }
		};

		[[nodiscard]] bool NormalizeFontSettings(
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
				defaultFontSizeMedium,
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

		[[nodiscard]] bool IsValidToggleKey(std::uint32_t a_key) noexcept
		{
			return FindBinding(keyboardBindings, a_key) != nullptr;
		}

		[[nodiscard]] bool IsValidGamePadToggleKey(std::uint32_t a_key) noexcept
		{
			return FindBinding(gamePadBindings, a_key) != nullptr;
		}

		[[nodiscard]] bool IsValidToggleMode(ToggleMode a_mode) noexcept
		{
			return std::to_underlying(a_mode) <= std::to_underlying(ToggleMode::Off);
		}

		[[nodiscard]] const wchar_t* ToggleModeName(ToggleMode a_mode) noexcept
		{
			for (const auto& mode : toggleModes) {
				if (mode.Value == a_mode) {
					return mode.Name;
				}
			}
			return toggleModes.front().Name;
		}

		[[nodiscard]] Values GetValues() noexcept
		{
			const StateLockGuard lock;
			return values;
		}

		void SetValues(const Values& a_values) noexcept
		{
			const StateLockGuard lock;
			values = a_values;
		}

		void Normalize(Values& a_values) noexcept
		{
			if (!IsValidToggleKey(a_values.ToggleKey)) {
				a_values.ToggleKey = defaultToggleKey;
			}
			if (!IsValidToggleMode(a_values.Mode)) {
				a_values.Mode = defaultToggleMode;
			}
			if (!IsValidGamePadToggleKey(a_values.ToggleKeyGamePad)) {
				a_values.ToggleKeyGamePad = defaultToggleKeyGamePad;
			}
			if (!IsValidToggleMode(a_values.ModeGamePad)) {
				a_values.ModeGamePad = defaultToggleModeGamePad;
			}
			if (!IsValidMenuStyleName(NameView(a_values.MenuStyle))) {
				a_values.MenuStyle = defaultMenuStyle;
			}
			static_cast<void>(NormalizeFontSettings(a_values.Fonts));
		}

		class Profile final
		{
		public:
			explicit Profile(const wchar_t* a_path) noexcept : path(a_path) {}

			template <class Value, class Parser>
			void Read(
				const wchar_t* a_section,
				const wchar_t* a_key,
				Value&         a_value,
				const Value&   a_default,
				Parser         a_parser) noexcept
			{
				std::array<wchar_t, valueCapacity> text{};
				const auto length = ::GetPrivateProfileStringW(
					a_section, a_key, missingValueSentinel, text.data(),
					static_cast<DWORD>(text.size()), path);
				if (length == 1 && text[0] == missingValueSentinel[0]) {
					return;
				}

				Value parsed = a_default;
				if (length == 0 || length >= text.size() - 1 ||
					!a_parser(text.data(), parsed)) {
					a_value = a_default;
					success = false;
					return;
				}
				a_value = parsed;
			}

			void Write(
				const wchar_t* a_section,
				const wchar_t* a_key,
				const wchar_t* a_value) noexcept
			{
				success = ::WritePrivateProfileStringW(
					a_section, a_key, a_value, path) != FALSE && success;
			}

			void Write(
				const wchar_t*   a_section,
				const wchar_t*   a_key,
				std::string_view a_value) noexcept
			{
				std::array<wchar_t, valueCapacity> text{};
				if (a_value.empty() || a_value.size() >= text.size()) {
					success = false;
					return;
				}
				for (std::size_t index = 0; index < a_value.size(); ++index) {
					const auto character = static_cast<unsigned char>(a_value[index]);
					if (character > 0x7F) {
						success = false;
						return;
					}
					text[index] = static_cast<wchar_t>(character);
				}
				Write(a_section, a_key, text.data());
			}

			void Write(
				const wchar_t* a_section,
				const wchar_t* a_key,
				float          a_value) noexcept
			{
				std::array<char, valueCapacity> text{};
				const auto result = std::to_chars(
					text.data(), text.data() + text.size() - 1, a_value,
					std::chars_format::general, 6);
				if (result.ec != std::errc{}) {
					success = false;
					return;
				}
				Write(a_section, a_key, std::string_view{
					text.data(), static_cast<std::size_t>(result.ptr - text.data()) });
			}

			[[nodiscard]] explicit operator bool() const noexcept { return success; }

		private:
			const wchar_t* path;
			bool           success{ true };
		};

		template <class Value>
		[[nodiscard]] Value ReadMember(Value Values::* a_member) noexcept
		{
			const StateLockGuard lock;
			return values.*a_member;
		}

		template <class Value>
		void WriteMember(Value Values::* a_member, const Value& a_value) noexcept
		{
			const StateLockGuard lock;
			values.*a_member = a_value;
		}

		template <class Value, class Validator>
		[[nodiscard]] bool WriteValidatedMember(
			Value Values::* a_member,
			Value           a_value,
			const Value&    a_default,
			Validator       a_validator) noexcept
		{
			const bool valid = a_validator(a_value);
			WriteMember(a_member, valid ? a_value : a_default);
			return valid;
		}

	}

	std::span<const Binding> GetKeyboardBindings() noexcept
	{
		return keyboardBindings;
	}

	std::span<const Binding> GetGamePadBindings() noexcept
	{
		return gamePadBindings;
	}

	std::string_view GetKeyboardBindingName(std::uint32_t a_key) noexcept
	{
		return BindingName(keyboardBindings, a_key);
	}

	std::string_view GetGamePadBindingName(std::uint32_t a_key) noexcept
	{
		return BindingName(gamePadBindings, a_key);
	}

	bool Load() noexcept
	{
		auto loaded = defaultValues;
		std::array<wchar_t, pathCapacity> path{};
		if (!BuildSettingsPath(path)) {
			SetValues(loaded);
			return false;
		}

		const auto attributes = ::GetFileAttributesW(path.data());
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			const auto error = ::GetLastError();
			SetValues(loaded);
			return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
		}
		if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			SetValues(loaded);
			return false;
		}

		Profile profile{ path.data() };
		profile.Read(
			sectionName, L"ToggleKey", loaded.ToggleKey,
			defaultToggleKey, [](std::wstring_view a_text, std::uint32_t& a_value) {
				return ParseBinding(a_text, keyboardBindings, a_value);
			});
		profile.Read(
			sectionName, L"ToggleMode", loaded.Mode,
			defaultToggleMode, ParseToggleMode);
		profile.Read(
			sectionName, L"ToggleKeyGamePad", loaded.ToggleKeyGamePad,
			defaultToggleKeyGamePad,
			[](std::wstring_view a_text, std::uint32_t& a_value) {
				return ParseBinding(a_text, gamePadBindings, a_value);
			});
		profile.Read(
			sectionName, L"ToggleModeGamePad", loaded.ModeGamePad,
			defaultToggleModeGamePad, ParseToggleMode);
		profile.Read(
			sectionName, L"FreezeTimeOnMenu", loaded.FreezeTimeOnMenu,
			defaultFreezeTimeOnMenu, ParseBool);
		profile.Read(
			sectionName, L"BlurBackgroundOnMenu", loaded.BlurBackgroundOnMenu,
			defaultBlurBackgroundOnMenu, ParseBool);
		profile.Read(
			sectionName, L"MenuStyle", loaded.MenuStyle,
			defaultMenuStyle, ParseMenuStyleName);
		profile.Read(
			fontSectionName, L"PrimaryFont", loaded.Fonts.PrimaryFont,
			defaultFontSettings.PrimaryFont, ParseFontFileName);
		for (const auto& setting : fontFloatSettings) {
			profile.Read(
				fontSectionName, setting.Name, loaded.Fonts.*setting.Value,
				setting.Default, ParseNumber<float>);
		}

		const bool valid = NormalizeFontSettings(loaded.Fonts) &&
			static_cast<bool>(profile);
		Normalize(loaded);
		SetValues(loaded);
		return valid;
	}

	bool Save() noexcept
	{
		auto saved = GetValues();
		Normalize(saved);

		std::array<wchar_t, pathCapacity> path{};
		if (!BuildSettingsPath(path)) {
			return false;
		}
		std::array<wchar_t, pathCapacity> temporaryPath{};
		if (!BuildTemporarySettingsPath(path, temporaryPath) ||
			!PrepareTemporarySettingsFile(path.data(), temporaryPath.data())) {
			return false;
		}

		Profile profile{ temporaryPath.data() };
		profile.Write(
			sectionName, L"ToggleKey", GetKeyboardBindingName(saved.ToggleKey));
		profile.Write(sectionName, L"ToggleMode", ToggleModeName(saved.Mode));
		profile.Write(
			sectionName, L"ToggleKeyGamePad",
			GetGamePadBindingName(saved.ToggleKeyGamePad));
		profile.Write(
			sectionName, L"ToggleModeGamePad", ToggleModeName(saved.ModeGamePad));
		profile.Write(
			sectionName, L"FreezeTimeOnMenu",
			saved.FreezeTimeOnMenu ? L"1" : L"0");
		profile.Write(
			sectionName, L"BlurBackgroundOnMenu",
			saved.BlurBackgroundOnMenu ? L"1" : L"0");
		profile.Write(sectionName, L"MenuStyle", saved.MenuStyle.data());
		profile.Write(
			fontSectionName, L"PrimaryFont", NameView(saved.Fonts.PrimaryFont));
		for (const auto& setting : fontFloatSettings) {
			profile.Write(
				fontSectionName, setting.Name, saved.Fonts.*setting.Value);
		}

		// The documented cache-flush form returns zero even when it only
		// flushes successfully, so its return value is not a failure signal.
		static_cast<void>(::WritePrivateProfileStringW(
			nullptr,
			nullptr,
			nullptr,
			temporaryPath.data()));
		bool success = static_cast<bool>(profile);
		if (success) {
			success = ::MoveFileExW(
				temporaryPath.data(),
				path.data(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		}
		if (!success) {
			const auto error = ::GetLastError();
			static_cast<void>(::DeleteFileW(temporaryPath.data()));
			::SetLastError(error);
			return false;
		}

		SetValues(saved);
		return true;
	}

	void ResetDefaults() noexcept { SetValues(defaultValues); }

	std::uint32_t GetToggleKey() noexcept { return ReadMember(&Values::ToggleKey); }
	ToggleMode GetToggleMode() noexcept { return ReadMember(&Values::Mode); }
	std::uint32_t GetToggleKeyGamePad() noexcept { return ReadMember(&Values::ToggleKeyGamePad); }
	ToggleMode GetToggleModeGamePad() noexcept { return ReadMember(&Values::ModeGamePad); }
	bool GetFreezeTimeOnMenu() noexcept { return ReadMember(&Values::FreezeTimeOnMenu); }
	bool GetBlurBackgroundOnMenu() noexcept { return ReadMember(&Values::BlurBackgroundOnMenu); }
	MenuStyleName GetMenuStyle() noexcept { return ReadMember(&Values::MenuStyle); }
	FontSettings GetFontSettings() noexcept { return ReadMember(&Values::Fonts); }
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
		       close(a_left.FontWeight, a_right.FontWeight) &&
		       close(a_left.FontSizeMedium, a_right.FontSizeMedium) &&
		       close(a_left.MinFontSize, a_right.MinFontSize) &&
		       close(a_left.MaxFontSize, a_right.MaxFontSize) &&
		       close(a_left.UIScale, a_right.UIScale);
	}

	bool ValidateFontSettings(const FontSettings& a_settings) noexcept { return IsValidFontSettings(a_settings); }

	bool SetToggleKey(std::uint32_t a_key) noexcept
	{
		return WriteValidatedMember(
			&Values::ToggleKey, a_key, defaultToggleKey, IsValidToggleKey);
	}

	bool SetToggleMode(ToggleMode a_mode) noexcept
	{
		return WriteValidatedMember(
			&Values::Mode, a_mode, defaultToggleMode, IsValidToggleMode);
	}

	bool SetToggleKeyGamePad(std::uint32_t a_key) noexcept
	{
		return WriteValidatedMember(
			&Values::ToggleKeyGamePad, a_key, defaultToggleKeyGamePad,
			IsValidGamePadToggleKey);
	}

	bool SetToggleModeGamePad(ToggleMode a_mode) noexcept
	{
		return WriteValidatedMember(
			&Values::ModeGamePad, a_mode, defaultToggleModeGamePad,
			IsValidToggleMode);
	}

	void SetFreezeTimeOnMenu(bool a_enabled) noexcept { WriteMember(&Values::FreezeTimeOnMenu, a_enabled); }
	void SetBlurBackgroundOnMenu(bool a_enabled) noexcept { WriteMember(&Values::BlurBackgroundOnMenu, a_enabled); }

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
