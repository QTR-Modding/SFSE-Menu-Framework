#pragma once

#include <cstddef>
#include <cstdint>

namespace RE
{
	class InputEvent;
}

namespace SFSEMenuFramework::BindingCapture
{
	enum class Device : std::uint8_t
	{
		Keyboard,
		Gamepad
	};

	enum class State : std::uint8_t
	{
		Idle,
		Waiting,
		Pressed,
		Complete,
		Cancelled,
		Confirming
	};

	inline constexpr std::uint32_t unboundKey = 0;

	struct NativeBatchResult final
	{
		bool OwnsBatch{};
		bool ForwardAllToImGui{};
		bool SuppressGamepadCancel{};
		bool Overflowed{};
	};

	void Begin(Device a_device) noexcept;
	void BeginConfirmation() noexcept;
	void Acknowledge() noexcept;
	void Abort() noexcept;

	[[nodiscard]] bool IsActive() noexcept;
	[[nodiscard]] bool IsConfirming() noexcept;
	[[nodiscard]] Device GetActiveDevice() noexcept;
	[[nodiscard]] State Poll(
		std::uint32_t& a_key, Device a_displayedDevice) noexcept;

	void ProcessKeyboardTransition(
		std::uint32_t a_dik,
		std::int32_t  a_engineEventID,
		bool          a_down) noexcept;
	void ObserveMouseActivity() noexcept;
	[[nodiscard]] NativeBatchResult ProcessNativeBatch(
		const RE::InputEvent* a_head, std::size_t a_limit) noexcept;
	[[nodiscard]] bool ShouldForwardKeyboardMessage(
		std::uint32_t a_virtualKey, bool a_down) noexcept;
}
