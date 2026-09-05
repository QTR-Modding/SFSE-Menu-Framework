#pragma once

#include <cstdint>

namespace RE
{
	class InputEvent;
}

namespace SFSEMenuFramework::GamepadNavigation
{
	struct RepeatTiming final
	{
		float Delay{};
		float Rate{};
	};

	void CaptureNativeEvent(
		const RE::InputEvent& a_event,
		std::uint64_t         a_generation,
		bool                  a_sendToImGui) noexcept;
	void ObserveGamepadActivity(std::uint64_t a_generation) noexcept;
	void ObserveMouseActivity(std::uint64_t a_generation) noexcept;
	[[nodiscard]] bool ShouldDrawMouseCursor(
		std::uint64_t a_generation) noexcept;
	void ApplyPending(std::uint64_t a_generation) noexcept;
	[[nodiscard]] RepeatTiming ApplyRepeatTiming() noexcept;
	void RestoreRepeatTiming(RepeatTiming a_timing) noexcept;
	[[nodiscard]] bool ConsumeCloseRequestForCurrentWindow(
		std::uint64_t a_generation) noexcept;
}
