#include "config/FrameworkSettings.h"
#include "config/FrameworkSettingsInternal.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <limits>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace SFSEMenuFramework::FrameworkSettings
{
	using namespace Detail;

	namespace
	{
		constexpr wchar_t sectionName[]{ L"General" };
		constexpr wchar_t fontSectionName[]{ L"Fonts" };
		constexpr wchar_t relativePath[]{ L"Data\\SFSE\\Plugins\\SFSEMenuFramework.ini" };
		constexpr wchar_t temporarySuffix[]{ L".tmp" };
		constexpr wchar_t missingValueSentinel[]{ L"\x1F" };
		constexpr std::size_t pathCapacity = 32768;
		constexpr std::size_t valueCapacity = FontFileName{}.size() + 1;
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

		[[nodiscard]] bool BuildSettingsPath(std::array<wchar_t, pathCapacity>& a_path) noexcept
		{
			const auto length = ::GetModuleFileNameW(
				nullptr, a_path.data(), static_cast<DWORD>(a_path.size()));
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
				a_path.data() + directoryLength, relativePath, suffixLength + 1);
			return true;
		}

		[[nodiscard]] bool BuildTemporarySettingsPath(
			const std::array<wchar_t, pathCapacity>& a_path,
			std::array<wchar_t, pathCapacity>&       a_temporaryPath) noexcept
		{
			return ::wcscpy_s(
				a_temporaryPath.data(), a_temporaryPath.size(), a_path.data()) == 0 &&
				::wcscat_s(
					a_temporaryPath.data(), a_temporaryPath.size(), temporarySuffix) == 0;
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
				a_temporaryPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
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
				narrow.data(), narrow.data() + a_text.size(), parsed);
			if (result.ec != std::errc{} ||
				result.ptr != narrow.data() + a_text.size() ||
				(std::is_floating_point_v<Value> && !std::isfinite(parsed))) {
				return false;
			}
			a_result = parsed;
			return true;
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

		[[nodiscard]] bool ParseToggleMode(
			std::wstring_view a_text, ToggleMode& a_result) noexcept
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

		[[nodiscard]] bool ParseFontRendering(
			std::wstring_view a_text, FontRendering& a_result) noexcept
		{
			a_text = Trim(a_text);
			for (const auto& rendering : fontRenderingModes) {
				if (EqualsIgnoreCase(a_text, rendering.Name)) {
					a_result = rendering.Value;
					return true;
				}
			}

			std::uint32_t numeric{};
			if (!ParseNumber(a_text, numeric) ||
				numeric > std::to_underlying(FontRendering::Auto)) {
				return false;
			}
			a_result = static_cast<FontRendering>(numeric);
			return true;
		}

		[[nodiscard]] bool ParseBool(std::wstring_view a_text, bool& a_result) noexcept
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
		class Profile final
		{
		public:
			explicit Profile(const wchar_t* a_path) noexcept : path(a_path) {}

			template <class Value, class Parser>
			void Read(
				const wchar_t* a_section, const wchar_t* a_key,
				Value& a_value, const Value& a_default, Parser a_parser) noexcept
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
				const wchar_t* a_section, const wchar_t* a_key, const wchar_t* a_value) noexcept
			{
				success = ::WritePrivateProfileStringW(
					a_section, a_key, a_value, path) != FALSE && success;
			}

			void Write(
				const wchar_t* a_section, const wchar_t* a_key, std::string_view a_value) noexcept
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

			void Write(const wchar_t* a_section, const wchar_t* a_key, float a_value) noexcept
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

		template <class Writer>
		[[nodiscard]] bool SaveProfile(Writer a_write) noexcept
		{
			std::array<wchar_t, pathCapacity> path{};
			std::array<wchar_t, pathCapacity> temporaryPath{};
			if (!BuildSettingsPath(path) ||
				!BuildTemporarySettingsPath(path, temporaryPath) ||
				!PrepareTemporarySettingsFile(path.data(), temporaryPath.data())) {
				return false;
			}

			Profile profile{ temporaryPath.data() };
			a_write(profile);
			// The cache-flush form returns zero even when it succeeds.
			static_cast<void>(::WritePrivateProfileStringW(
				nullptr, nullptr, nullptr, temporaryPath.data()));
			if (static_cast<bool>(profile) && ::MoveFileExW(
					temporaryPath.data(), path.data(),
					MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
				return true;
			}
			const auto error = ::GetLastError();
			static_cast<void>(::DeleteFileW(temporaryPath.data()));
			::SetLastError(error);
			return false;
		}
	}

	bool LoadWindowLayout(WindowLayout& a_layout) noexcept
	{
		std::array<wchar_t, pathCapacity> path{};
		if (!a_layout.Section || !BuildSettingsPath(path)) {
			return false;
		}
		constexpr float missing = std::numeric_limits<float>::quiet_NaN();
		WindowLayout loaded{ a_layout.Section, missing, missing, missing, missing };
		Profile profile{ path.data() };
		profile.Read(loaded.Section, L"X", loaded.X, missing, ParseNumber<float>);
		profile.Read(loaded.Section, L"Y", loaded.Y, missing, ParseNumber<float>);
		profile.Read(loaded.Section, L"Width", loaded.Width, missing, ParseNumber<float>);
		profile.Read(loaded.Section, L"Height", loaded.Height, missing, ParseNumber<float>);
		if (!profile || !std::isfinite(loaded.X) || !std::isfinite(loaded.Y) ||
			!std::isfinite(loaded.Width) || !std::isfinite(loaded.Height) ||
			loaded.Width <= 0.0F || loaded.Height <= 0.0F) {
			return false;
		}
		a_layout = loaded;
		return true;
	}

	bool SaveWindowLayouts(std::span<const WindowLayout> a_layouts) noexcept
	{
		for (const auto& layout : a_layouts) {
			if (!layout.Section || !std::isfinite(layout.X) || !std::isfinite(layout.Y) ||
				!std::isfinite(layout.Width) || !std::isfinite(layout.Height) ||
				layout.Width <= 0.0F || layout.Height <= 0.0F) {
				return false;
			}
		}
		return a_layouts.empty() || SaveProfile([a_layouts](Profile& profile) {
			for (const auto& layout : a_layouts) {
				profile.Write(layout.Section, L"X", layout.X);
				profile.Write(layout.Section, L"Y", layout.Y);
				profile.Write(layout.Section, L"Width", layout.Width);
				profile.Write(layout.Section, L"Height", layout.Height);
			}
		});
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
		profile.Read(sectionName, L"ToggleKey", loaded.ToggleKey, defaultValues.ToggleKey,
			[](std::wstring_view a_text, std::uint32_t& a_value) {
				return ParseBinding(a_text, keyboardBindings, a_value);
			});
		profile.Read(sectionName, L"ToggleMode", loaded.Mode, defaultValues.Mode, ParseToggleMode);
		profile.Read(sectionName, L"ToggleKeyGamePad", loaded.ToggleKeyGamePad,
			defaultValues.ToggleKeyGamePad,
			[](std::wstring_view a_text, std::uint32_t& a_value) {
				return ParseBinding(a_text, gamePadBindings, a_value);
			});
		profile.Read(sectionName, L"ToggleModeGamePad", loaded.ModeGamePad,
			defaultValues.ModeGamePad, ParseToggleMode);
		profile.Read(sectionName, L"FreezeTimeOnMenu", loaded.FreezeTimeOnMenu,
			defaultValues.FreezeTimeOnMenu, ParseBool);
		profile.Read(sectionName, L"BlurBackgroundOnMenu", loaded.BlurBackgroundOnMenu,
			defaultValues.BlurBackgroundOnMenu, ParseBool);
		for (const auto& setting : backgroundSettings) {
			profile.Read(sectionName, setting.Name, loaded.*setting.Member,
				defaultValues.*setting.Member, [](std::wstring_view text, float& value) {
					return ParseNumber(text, value) && value >= 0.0F && value <= 1.0F;
				});
		}
		profile.Read(sectionName, L"MenuStyle", loaded.MenuStyle,
			defaultValues.MenuStyle, [](std::wstring_view text, MenuStyleName& value) {
				return ParseName(text, value, true, false);
			});
		profile.Read(fontSectionName, L"PrimaryFont", loaded.Fonts.PrimaryFont,
			defaultFontSettings.PrimaryFont, [](std::wstring_view text, FontFileName& value) {
				return ParseName(text, value, false, true);
			});
		profile.Read(fontSectionName, L"FontRendering", loaded.Fonts.Rendering,
			defaultFontSettings.Rendering, ParseFontRendering);
		for (const auto& setting : fontFloatSettings) {
			profile.Read(
				fontSectionName, setting.Name, loaded.Fonts.*setting.Value,
				setting.Default, ParseNumber<float>);
		}
		for (const auto& setting : glyphSettings) {
			profile.Read(
				fontSectionName, setting.Name, loaded.Fonts.Glyphs.*setting.Value,
				defaultFontSettings.Glyphs.*setting.Value, ParseBool);
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
		const bool success = SaveProfile([&saved](Profile& profile) {
			profile.Write(sectionName, L"ToggleKey", GetKeyboardBindingName(saved.ToggleKey));
			profile.Write(sectionName, L"ToggleMode", ToggleModeName(saved.Mode));
			profile.Write(sectionName, L"ToggleKeyGamePad",
				GetGamePadBindingName(saved.ToggleKeyGamePad));
			profile.Write(sectionName, L"ToggleModeGamePad", ToggleModeName(saved.ModeGamePad));
			profile.Write(sectionName, L"FreezeTimeOnMenu",
				saved.FreezeTimeOnMenu ? L"1" : L"0");
			profile.Write(sectionName, L"BlurBackgroundOnMenu",
				saved.BlurBackgroundOnMenu ? L"1" : L"0");
			for (const auto& setting : backgroundSettings) {
				profile.Write(sectionName, setting.Name, saved.*setting.Member);
			}
			profile.Write(sectionName, L"MenuStyle", saved.MenuStyle.data());
			profile.Write(fontSectionName, L"PrimaryFont", NameView(saved.Fonts.PrimaryFont));
			profile.Write(fontSectionName, L"FontRendering",
				GetFontRenderingName(saved.Fonts.Rendering));
			for (const auto& setting : fontFloatSettings) {
				profile.Write(
					fontSectionName, setting.Name, saved.Fonts.*setting.Value);
			}
			for (const auto& setting : glyphSettings) {
				profile.Write(
					fontSectionName, setting.Name,
					saved.Fonts.Glyphs.*setting.Value ? L"1" : L"0");
			}
		});
		if (!success) {
			return false;
		}

		SetValues(saved);
		return true;
	}
	std::filesystem::path BuildGamePath(std::wstring_view a_relativePath)
	{
		std::array<wchar_t, pathCapacity> executablePath{};
		const auto length = ::GetModuleFileNameW(
			nullptr,
			executablePath.data(),
			static_cast<DWORD>(executablePath.size()));
		if (length == 0 || length >= executablePath.size()) {
			return {};
		}
		return std::filesystem::path(executablePath.data()).parent_path() /
			a_relativePath;
	}
}
