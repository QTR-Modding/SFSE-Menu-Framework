#pragma once

#include <cstdint>
#include <span>
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

	struct Binding final
	{
		std::string_view Name;
		std::uint32_t   Code;
	};

	[[nodiscard]] bool Load() noexcept;
	[[nodiscard]] bool Save() noexcept;
	void               ResetDefaults() noexcept;

	[[nodiscard]] std::span<const Binding> GetKeyboardBindings() noexcept;
	[[nodiscard]] std::span<const Binding> GetGamePadBindings() noexcept;
	[[nodiscard]] std::string_view GetKeyboardBindingName(
		std::uint32_t a_key) noexcept;
	[[nodiscard]] std::string_view GetGamePadBindingName(
		std::uint32_t a_key) noexcept;

	[[nodiscard]] std::uint32_t GetToggleKey() noexcept;
	[[nodiscard]] ToggleMode    GetToggleMode() noexcept;
	[[nodiscard]] std::uint32_t GetToggleKeyGamePad() noexcept;
	[[nodiscard]] ToggleMode    GetToggleModeGamePad() noexcept;
	[[nodiscard]] bool          GetFreezeTimeOnMenu() noexcept;
	[[nodiscard]] bool          GetBlurBackgroundOnMenu() noexcept;

	[[nodiscard]] bool SetToggleKey(std::uint32_t a_key) noexcept;
	[[nodiscard]] bool SetToggleMode(ToggleMode a_mode) noexcept;
	[[nodiscard]] bool SetToggleKeyGamePad(std::uint32_t a_key) noexcept;
	[[nodiscard]] bool SetToggleModeGamePad(ToggleMode a_mode) noexcept;
	void               SetFreezeTimeOnMenu(bool a_enabled) noexcept;
	void               SetBlurBackgroundOnMenu(bool a_enabled) noexcept;
}
