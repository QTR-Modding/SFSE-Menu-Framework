#pragma once

#include "platform/win32/Win32Platform.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace SFSEMenuFramework::Win32Platform::Detail
{
	inline constexpr std::size_t rawMouseMessageCapacity = 13;

	enum class PointerRoute : std::uint8_t
	{
		Disabled,
		EarlyRaw,
		Legacy
	};

	struct QueuedWindowMessage final
	{
		HWND   Window{ nullptr };
		UINT   Message{ 0 };
		WPARAM WParam{ 0 };
		LPARAM LParam{ 0 };
	};

	struct SharedState final
	{
		std::atomic<bool>               SubclassActive{ false };
		std::atomic<bool>               AcceptInput{ false };
		std::atomic<bool>               BackendAlive{ false };
		std::atomic<bool>               HostWindowTearingDown{ false };
		std::atomic<bool>               HostCallbackPending{ false };
		std::atomic<HWND>               InitializedHostWindow{ nullptr };
		std::atomic<DWORD>              InitializedHostWindowThreadID{ 0 };
		std::atomic<HostWindowCallback> HostWindowCallbackFunction{ nullptr };
		std::atomic<std::uint64_t>      InputStateGeneration{ 1 };
		std::atomic<PointerRoute>       PointerRouteState{ PointerRoute::Disabled };
		std::atomic<std::uint64_t>      EarlyRawMouseGeneration{ 0 };
		std::atomic<std::uint64_t>      RawMouseFaultGeneration{ 0 };
	};

	[[nodiscard]] SharedState& Shared() noexcept;

	[[nodiscard]] inline bool FirstFailure(
		std::atomic_flag& a_flag) noexcept
	{
		return !a_flag.test_and_set(std::memory_order_relaxed);
	}

	[[nodiscard]] inline bool IsKeyMessage(UINT a_message) noexcept
	{
		return a_message == WM_KEYDOWN || a_message == WM_KEYUP ||
			a_message == WM_SYSKEYDOWN || a_message == WM_SYSKEYUP;
	}

	[[nodiscard]] bool ReadClientArea(
		HWND a_window, RECT& a_area) noexcept;
	[[nodiscard]] bool HasCurrentInputLease() noexcept;

	void ResetKeyboardToggleState() noexcept;
	[[nodiscard]] bool ProcessKeyboardHoldTimer(
		HWND a_window, UINT_PTR a_timerID) noexcept;
	void ProcessRawKeyboard(HWND a_window, const RAWKEYBOARD& a_keyboard) noexcept;
	void ProcessLegacyKeyboard(
		HWND a_window, UINT a_message, WPARAM a_wParam,
		LPARAM a_lParam) noexcept;

	void ResetRawMouseState() noexcept;
	void DeactivateRawMouseState() noexcept;
	void ResetWindowThreadMouseState(HWND a_window);
	void UpdateWindowThreadMouseState(
		HWND a_window, UINT a_message, WPARAM a_wParam);
	void HandleCaptureChanged(HWND a_window);
	void ResetCapturedMouse(HWND a_window);
	[[nodiscard]] bool ProcessEarlyRawMouse(
		HWND a_window, const RAWMOUSE& a_mouse) noexcept;
	[[nodiscard]] bool InitializeEarlyRawMouse(
		HWND a_window, std::uint64_t a_generation,
		QueuedWindowMessage& a_seedMessage) noexcept;
	[[nodiscard]] bool IsEarlyRawMouseReady(
		HWND a_window, std::uint64_t a_generation) noexcept;
	[[nodiscard]] bool PlaceLegacyCursorForRawHandoff(
		HWND a_window) noexcept;

	void RequestInputReset();
	[[nodiscard]] bool EnqueueWindowMessage(
		HWND a_window, UINT a_message, WPARAM a_wParam,
		LPARAM a_lParam, bool a_requiresAcceptedInput);
	[[nodiscard]] bool EnqueueRawMouseBatch(
		const std::array<QueuedWindowMessage, rawMouseMessageCapacity>& a_messages,
		std::size_t a_count, std::uint64_t a_generation);
}
