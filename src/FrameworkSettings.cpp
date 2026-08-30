#include "FrameworkSettings.h"

#include <REX/W32/DINPUT.h>
#include <REX/W32/XINPUT.h>

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
		// The option names, toggle-mode values, defaults, and persistent INI
		// behavior are adapted from SKSE Menu Framework 3 at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0). This is an
		// independent Win32-profile implementation for Starfield; no Skyrim
		// engine code, theme settings, or font settings are copied.
		constexpr std::uint32_t defaultToggleKey = REX::W32::DIK_F1;
		constexpr ToggleMode    defaultToggleMode = ToggleMode::SinglePress;
		constexpr std::uint32_t defaultToggleKeyGamePad =
			REX::W32::XINPUT_GAMEPAD_LEFT_SHOULDER;
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
			return a_key > 0 && a_key <= 0xFF &&
			       a_key != REX::W32::DIK_ESCAPE &&
			       a_key != REX::W32::DIK_SYSRQ;
		}

		[[nodiscard]] bool IsValidGamePadToggleKey(std::uint32_t a_key) noexcept
		{
			switch (a_key) {
			case REX::W32::XINPUT_GAMEPAD_DPAD_UP:
			case REX::W32::XINPUT_GAMEPAD_DPAD_DOWN:
			case REX::W32::XINPUT_GAMEPAD_DPAD_LEFT:
			case REX::W32::XINPUT_GAMEPAD_DPAD_RIGHT:
			case REX::W32::XINPUT_GAMEPAD_START:
			case REX::W32::XINPUT_GAMEPAD_BACK:
			case REX::W32::XINPUT_GAMEPAD_LEFT_THUMB:
			case REX::W32::XINPUT_GAMEPAD_RIGHT_THUMB:
			case REX::W32::XINPUT_GAMEPAD_LEFT_SHOULDER:
			case REX::W32::XINPUT_GAMEPAD_RIGHT_SHOULDER:
			case REX::W32::XINPUT_GAMEPAD_A:
			case REX::W32::XINPUT_GAMEPAD_B:
			case REX::W32::XINPUT_GAMEPAD_X:
			case REX::W32::XINPUT_GAMEPAD_Y:
			case 0x9:  // Starfield's left-trigger ButtonEvent ID.
			case 0xA:  // Starfield's right-trigger ButtonEvent ID.
				return true;
			default:
				return false;
			}
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

		[[nodiscard]] bool WriteUnsigned(
			const wchar_t* a_path,
			const wchar_t* a_key,
			std::uint32_t  a_value) noexcept
		{
			std::array<wchar_t, 16> text{};
			if (_snwprintf_s(
					text.data(),
					text.size(),
					_TRUNCATE,
					L"%u",
					a_value) < 0) {
				return false;
			}
			return ::WritePrivateProfileStringW(
				sectionName,
				a_key,
				text.data(),
				a_path) != FALSE;
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
			if (ParseUnsigned(text.data(), key) && IsValidToggleKey(key)) {
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
			if (ParseUnsigned(text.data(), key) && IsValidGamePadToggleKey(key)) {
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
		success = WriteUnsigned(path.data(), L"ToggleKey", saved.ToggleKey) && success;
		success = WriteText(path.data(), L"ToggleMode", ToggleModeName(saved.Mode)) && success;
		success = WriteUnsigned(
			path.data(),
			L"ToggleKeyGamePad",
			saved.ToggleKeyGamePad) && success;
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

		const bool flushed = ::WritePrivateProfileStringW(
			nullptr,
			nullptr,
			nullptr,
			path.data()) != FALSE;
		return success && flushed;
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
