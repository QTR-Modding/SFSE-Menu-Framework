#include "FrameworkSettings.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cwchar>
#include <iterator>
#include <limits>
#include <string_view>
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
		constexpr std::uint32_t defaultToggleKeyGamePad = 256;
		constexpr ToggleMode defaultToggleModeGamePad = ToggleMode::DoublePress;
		constexpr bool       defaultFreezeTimeOnMenu = true;
		constexpr bool       defaultBlurBackgroundOnMenu = true;

		constexpr wchar_t sectionName[]{ L"General" };
		constexpr wchar_t relativePath[]{
			L"Data\\SFSE\\Plugins\\SFSEMenuFramework.ini"
		};
		constexpr wchar_t missingValueSentinel[]{ L"\x1F" };
		constexpr std::size_t pathCapacity = 32768;
		constexpr std::size_t valueCapacity = 64;

		struct Values final
		{
			std::uint32_t ToggleKey{ defaultToggleKey };
			ToggleMode    Mode{ defaultToggleMode };
			std::uint32_t ToggleKeyGamePad{ defaultToggleKeyGamePad };
			ToggleMode    ModeGamePad{ defaultToggleModeGamePad };
			bool          FreezeTimeOnMenu{ defaultFreezeTimeOnMenu };
			bool          BlurBackgroundOnMenu{ defaultBlurBackgroundOnMenu };
		};

		class StateLockGuard final
		{
		public:
			StateLockGuard() noexcept;
			~StateLockGuard();

			StateLockGuard(const StateLockGuard&) = delete;
			StateLockGuard(StateLockGuard&&) = delete;
			StateLockGuard& operator=(const StateLockGuard&) = delete;
			StateLockGuard& operator=(StateLockGuard&&) = delete;
		};

		enum class ReadResult : std::uint8_t
		{
			Missing,
			Present,
			Invalid
		};

		std::atomic_flag stateLock{};
		Values           values{};

		StateLockGuard::StateLockGuard() noexcept
		{
			while (stateLock.test_and_set(std::memory_order_acquire)) {
				stateLock.wait(true, std::memory_order_relaxed);
			}
		}

		StateLockGuard::~StateLockGuard()
		{
			stateLock.clear(std::memory_order_release);
			stateLock.notify_one();
		}

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

		[[nodiscard]] wchar_t ToUpperAscii(wchar_t a_character) noexcept
		{
			return a_character >= L'a' && a_character <= L'z' ?
				a_character - (L'a' - L'A') :
				a_character;
		}

		[[nodiscard]] bool EqualsIgnoreCase(
			std::wstring_view a_left,
			std::wstring_view a_right) noexcept
		{
			if (a_left.size() != a_right.size()) {
				return false;
			}
			for (std::size_t index = 0; index < a_left.size(); ++index) {
				if (ToUpperAscii(a_left[index]) != ToUpperAscii(a_right[index])) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool EqualsIgnoreCase(
			std::wstring_view a_left,
			std::string_view  a_right) noexcept
		{
			if (a_left.size() != a_right.size()) {
				return false;
			}
			for (std::size_t index = 0; index < a_left.size(); ++index) {
				const auto right = static_cast<unsigned char>(a_right[index]);
				if (right > 0x7F ||
					ToUpperAscii(a_left[index]) !=
						ToUpperAscii(static_cast<wchar_t>(right))) {
					return false;
				}
			}
			return true;
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

		[[nodiscard]] ReadResult ReadSetting(
			const wchar_t*                        a_path,
			const wchar_t*                        a_key,
			std::array<wchar_t, valueCapacity>& a_value) noexcept
		{
			const auto length = ::GetPrivateProfileStringW(
				sectionName,
				a_key,
				missingValueSentinel,
				a_value.data(),
				static_cast<DWORD>(a_value.size()),
				a_path);
			if (length >= a_value.size() - 1) {
				return ReadResult::Invalid;
			}
			if (length == 1 && a_value[0] == missingValueSentinel[0]) {
				return ReadResult::Missing;
			}
			return length == 0 ? ReadResult::Invalid : ReadResult::Present;
		}

		[[nodiscard]] bool ParseUnsigned(
			std::wstring_view a_text,
			std::uint32_t&    a_result) noexcept
		{
			a_text = Trim(a_text);
			if (a_text.empty()) {
				return false;
			}

			std::uint64_t value{};
			for (const auto character : a_text) {
				if (character < L'0' || character > L'9') {
					return false;
				}
				value = value * 10 + static_cast<std::uint64_t>(character - L'0');
				if (value > (std::numeric_limits<std::uint32_t>::max)()) {
					return false;
				}
			}

			a_result = static_cast<std::uint32_t>(value);
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
			if (ParseUnsigned(a_text, numeric) &&
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
			if (EqualsIgnoreCase(a_text, L"SINGLEPRESS")) {
				a_result = ToggleMode::SinglePress;
				return true;
			}
			if (EqualsIgnoreCase(a_text, L"HOLD")) {
				a_result = ToggleMode::Hold;
				return true;
			}
			if (EqualsIgnoreCase(a_text, L"DOUBLEPRESS")) {
				a_result = ToggleMode::DoublePress;
				return true;
			}
			if (EqualsIgnoreCase(a_text, L"OFF")) {
				a_result = ToggleMode::Off;
				return true;
			}

			std::uint32_t numeric{};
			if (!ParseUnsigned(a_text, numeric) ||
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
			switch (a_mode) {
			case ToggleMode::SinglePress:
				return L"SINGLEPRESS";
			case ToggleMode::Hold:
				return L"HOLD";
			case ToggleMode::DoublePress:
				return L"DOUBLEPRESS";
			case ToggleMode::Off:
				return L"OFF";
			default:
				return L"SINGLEPRESS";
			}
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
		}

		[[nodiscard]] bool WriteText(
			const wchar_t* a_path,
			const wchar_t* a_key,
			const wchar_t* a_value) noexcept
		{
			return ::WritePrivateProfileStringW(
				sectionName,
				a_key,
				a_value,
				a_path) != FALSE;
		}

		[[nodiscard]] bool WriteAsciiText(
			const wchar_t*   a_path,
			const wchar_t*   a_key,
			std::string_view a_value) noexcept
		{
			std::array<wchar_t, valueCapacity> text{};
			if (a_value.empty() || a_value.size() >= text.size()) {
				return false;
			}
			for (std::size_t index = 0; index < a_value.size(); ++index) {
				const auto character = static_cast<unsigned char>(a_value[index]);
				if (character > 0x7F) {
					return false;
				}
				text[index] = static_cast<wchar_t>(character);
			}
			return WriteText(a_path, a_key, text.data());
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
		Values loaded{};
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

		bool valid = true;
		std::array<wchar_t, valueCapacity> text{};
		auto result = ReadSetting(path.data(), L"ToggleKey", text);
		if (result == ReadResult::Present) {
			std::uint32_t key{};
			if (ParseBinding(text.data(), keyboardBindings, key)) {
				loaded.ToggleKey = key;
			} else {
				valid = false;
			}
		} else if (result == ReadResult::Invalid) {
			valid = false;
		}

		result = ReadSetting(path.data(), L"ToggleMode", text);
		if (result == ReadResult::Present) {
			if (!ParseToggleMode(text.data(), loaded.Mode)) {
				loaded.Mode = defaultToggleMode;
				valid = false;
			}
		} else if (result == ReadResult::Invalid) {
			valid = false;
		}

		result = ReadSetting(path.data(), L"ToggleKeyGamePad", text);
		if (result == ReadResult::Present) {
			std::uint32_t key{};
			if (ParseBinding(text.data(), gamePadBindings, key)) {
				loaded.ToggleKeyGamePad = key;
			} else {
				valid = false;
			}
		} else if (result == ReadResult::Invalid) {
			valid = false;
		}

		result = ReadSetting(path.data(), L"ToggleModeGamePad", text);
		if (result == ReadResult::Present) {
			if (!ParseToggleMode(text.data(), loaded.ModeGamePad)) {
				loaded.ModeGamePad = defaultToggleModeGamePad;
				valid = false;
			}
		} else if (result == ReadResult::Invalid) {
			valid = false;
		}

		result = ReadSetting(path.data(), L"FreezeTimeOnMenu", text);
		if (result == ReadResult::Present) {
			if (!ParseBool(text.data(), loaded.FreezeTimeOnMenu)) {
				loaded.FreezeTimeOnMenu = defaultFreezeTimeOnMenu;
				valid = false;
			}
		} else if (result == ReadResult::Invalid) {
			valid = false;
		}

		result = ReadSetting(path.data(), L"BlurBackgroundOnMenu", text);
		if (result == ReadResult::Present) {
			if (!ParseBool(text.data(), loaded.BlurBackgroundOnMenu)) {
				loaded.BlurBackgroundOnMenu = defaultBlurBackgroundOnMenu;
				valid = false;
			}
		} else if (result == ReadResult::Invalid) {
			valid = false;
		}

		Normalize(loaded);
		SetValues(loaded);
		return valid;
	}

	bool Save() noexcept
	{
		auto saved = GetValues();
		Normalize(saved);
		SetValues(saved);

		std::array<wchar_t, pathCapacity> path{};
		if (!BuildSettingsPath(path)) {
			return false;
		}

		bool success = true;
		success = WriteAsciiText(
			path.data(),
			L"ToggleKey",
			GetKeyboardBindingName(saved.ToggleKey)) && success;
		success = WriteText(path.data(), L"ToggleMode", ToggleModeName(saved.Mode)) && success;
		success = WriteAsciiText(
			path.data(),
			L"ToggleKeyGamePad",
			GetGamePadBindingName(saved.ToggleKeyGamePad)) && success;
		success = WriteText(
			path.data(),
			L"ToggleModeGamePad",
			ToggleModeName(saved.ModeGamePad)) && success;
		success = WriteText(
			path.data(),
			L"FreezeTimeOnMenu",
			saved.FreezeTimeOnMenu ? L"1" : L"0") && success;
		success = WriteText(
			path.data(),
			L"BlurBackgroundOnMenu",
			saved.BlurBackgroundOnMenu ? L"1" : L"0") && success;

		static_cast<void>(::WritePrivateProfileStringW(
			nullptr,
			nullptr,
			nullptr,
			path.data()));
		return success;
	}

	void ResetDefaults() noexcept
	{
		SetValues(Values{});
	}

	std::uint32_t GetToggleKey() noexcept
	{
		return GetValues().ToggleKey;
	}

	ToggleMode GetToggleMode() noexcept
	{
		return GetValues().Mode;
	}

	std::uint32_t GetToggleKeyGamePad() noexcept
	{
		return GetValues().ToggleKeyGamePad;
	}

	ToggleMode GetToggleModeGamePad() noexcept
	{
		return GetValues().ModeGamePad;
	}

	bool GetFreezeTimeOnMenu() noexcept
	{
		return GetValues().FreezeTimeOnMenu;
	}

	bool GetBlurBackgroundOnMenu() noexcept
	{
		return GetValues().BlurBackgroundOnMenu;
	}

	bool SetToggleKey(std::uint32_t a_key) noexcept
	{
		const bool valid = IsValidToggleKey(a_key);
		const StateLockGuard lock;
		values.ToggleKey = valid ? a_key : defaultToggleKey;
		return valid;
	}

	bool SetToggleMode(ToggleMode a_mode) noexcept
	{
		const bool valid = IsValidToggleMode(a_mode);
		const StateLockGuard lock;
		values.Mode = valid ? a_mode : defaultToggleMode;
		return valid;
	}

	bool SetToggleKeyGamePad(std::uint32_t a_key) noexcept
	{
		const bool valid = IsValidGamePadToggleKey(a_key);
		const StateLockGuard lock;
		values.ToggleKeyGamePad = valid ? a_key : defaultToggleKeyGamePad;
		return valid;
	}

	bool SetToggleModeGamePad(ToggleMode a_mode) noexcept
	{
		const bool valid = IsValidToggleMode(a_mode);
		const StateLockGuard lock;
		values.ModeGamePad = valid ? a_mode : defaultToggleModeGamePad;
		return valid;
	}

	void SetFreezeTimeOnMenu(bool a_enabled) noexcept
	{
		const StateLockGuard lock;
		values.FreezeTimeOnMenu = a_enabled;
	}

	void SetBlurBackgroundOnMenu(bool a_enabled) noexcept
	{
		const StateLockGuard lock;
		values.BlurBackgroundOnMenu = a_enabled;
	}
}
