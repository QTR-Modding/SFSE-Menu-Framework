#include "platform/win32/Win32PlatformInternal.h"

#include "config/FrameworkSettings.h"
#include "input/InputCapture.h"
#include "runtime/WindowManager.h"

#include <REX/W32/DINPUT.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace SFSEMenuFramework::Win32Platform
{
	using namespace Detail;

	namespace
	{
		constexpr auto doublePressThreshold = std::chrono::milliseconds{ 300 };
		constexpr UINT keyboardHoldThresholdMilliseconds = 401;
		constexpr UINT_PTR keyboardHoldTimerTag =
			static_cast<UINT_PTR>(0x53464D4600000000ULL);
		constexpr UINT_PTR keyboardHoldTimerTagMask =
			static_cast<UINT_PTR>(0xFFFFFFFF00000000ULL);
		struct KeyboardTransition final
		{
			std::uint32_t DIK{ 0 };
			std::int32_t  EngineEventID{ -1 };
			bool          Down{ false };
		};

		struct KeyboardToggleState final
		{
			std::array<bool, 0x100>              Down{};
			std::chrono::steady_clock::time_point LastPress{};
			std::uint32_t                         LastPressDIK{ 0 };
			std::uint32_t                         HoldDIK{ 0 };
			std::int32_t                          HoldEngineEventID{ -1 };
			HWND                                  HoldWindow{ nullptr };
			UINT_PTR                              HoldTimerID{ 0 };
			bool                                  HasLastPress{ false };
		};
		std::atomic_flag                keyboardHoldTimerFailureLogged{};
		std::atomic<std::uint32_t>      keyboardHoldTimerGeneration{ 0 };

		template <class T>
		[[nodiscard]] T& State()
		{
			static auto* state = new T();
			return *state;
		}

		void CancelKeyboardHold() noexcept
		{
			auto& state = State<KeyboardToggleState>();
			if (state.HoldTimerID != 0 && state.HoldWindow) {
				static_cast<void>(::KillTimer(state.HoldWindow, state.HoldTimerID));
			}
			state.HoldDIK = 0;
			state.HoldEngineEventID = -1;
			state.HoldWindow = nullptr;
			state.HoldTimerID = 0;
			InputCapture::CancelPendingHeldKeyboardSuppression();
		}
	}

	void Detail::ResetKeyboardToggleState() noexcept
	{
		auto& state = State<KeyboardToggleState>();
		CancelKeyboardHold();
		InputCapture::CancelPendingKeyboardSuppression();
		state.Down.fill(false);
		state.LastPress = {};
		state.LastPressDIK = 0;
		state.HasLastPress = false;
	}

	namespace
	{
		[[nodiscard]] std::optional<std::int32_t>
		NormalizeStarfieldKeyboardEventID(
			USHORT a_makeCode,
			USHORT a_flags,
			USHORT a_virtualKey) noexcept
		{
			// Mirrors Starfield 1.16.244's RAWKEYBOARD normalization at
			// 0x1422D7AFF-0x1422D7CF2. This is used only to correlate the
			// lossless DIK decision with the exact later engine batch.
			std::uint32_t id = a_virtualKey;
			if (id == 0xFF) {
				return std::nullopt;
			}

			if (id == VK_SHIFT) {
				id = ::MapVirtualKeyA(a_makeCode, MAPVK_VSC_TO_VK_EX);
			} else if (id == VK_NUMLOCK) {
				id = ::MapVirtualKeyA(VK_NUMLOCK, MAPVK_VK_TO_VSC);
			}

			const bool e0 = (a_flags & RI_KEY_E0) != 0;
			const bool e1 = (a_flags & RI_KEY_E1) != 0;
			if (e1 && id != VK_PAUSE) {
				id = ::MapVirtualKeyA(id, MAPVK_VK_TO_VSC);
			}

			if (id == VK_CONTROL) {
				id = VK_LCONTROL + static_cast<std::uint32_t>(e0);
			} else if (id == VK_MENU) {
				id = VK_LMENU + static_cast<std::uint32_t>(e0);
			} else if (!e0) {
				switch (id) {
				case VK_CLEAR:  id = VK_NUMPAD5; break;
				case VK_PRIOR:  id = VK_NUMPAD9; break;
				case VK_NEXT:   id = VK_NUMPAD3; break;
				case VK_END:    id = VK_NUMPAD1; break;
				case VK_HOME:   id = VK_NUMPAD7; break;
				case VK_LEFT:   id = VK_NUMPAD4; break;
				case VK_UP:     id = VK_NUMPAD8; break;
				case VK_RIGHT:  id = VK_NUMPAD6; break;
				case VK_DOWN:   id = VK_NUMPAD2; break;
				case VK_INSERT: id = VK_NUMPAD0; break;
				case VK_DELETE: id = VK_DECIMAL; break;
				default:
					break;
				}
			}

			if (id == 0 || id >= 0x100) {
				return std::nullopt;
			}
			return static_cast<std::int32_t>(id);
		}
		[[nodiscard]] std::optional<KeyboardTransition> DecodeKeyboard(
			USHORT a_makeCode,
			USHORT a_flags,
			USHORT a_virtualKey,
			std::uint32_t a_dik = 0) noexcept
		{
			if (a_dik == 0) {
				if ((a_flags & RI_KEY_E1) != 0) {
					if (a_makeCode != 0x45 || a_virtualKey != VK_PAUSE) {
						return std::nullopt;
					}
					a_dik = REX::W32::DIK_PAUSE;
				} else {
					a_dik = a_makeCode & 0x7FU;
					if ((a_flags & RI_KEY_E0) != 0) {
						a_dik |= 0x80U;
					}
				}
			}
			if (a_dik == 0 || a_dik >= 0x100) {
				return std::nullopt;
			}

			const auto engineEventID =
				NormalizeStarfieldKeyboardEventID(a_makeCode, a_flags, a_virtualKey);
			return KeyboardTransition{
				.DIK = a_dik,
				.EngineEventID = engineEventID.value_or(-1),
				.Down = (a_flags & RI_KEY_BREAK) == 0
			};
		}
		[[nodiscard]] std::optional<KeyboardTransition> ReadLegacyKeyboard(
			UINT a_message,
			WPARAM a_wParam,
			LPARAM a_lParam) noexcept
		{
			if (!IsKeyMessage(a_message)) {
				return std::nullopt;
			}

			const auto bits = static_cast<std::uintptr_t>(a_lParam);
			const auto makeCode =
				static_cast<USHORT>((bits >> 16) & 0x7F);
			USHORT flags{};
			if ((bits & (std::uintptr_t{ 1 } << 24)) != 0) {
				flags |= RI_KEY_E0;
			}
			if (a_wParam == VK_PAUSE) {
				flags |= RI_KEY_E1;
			}
			if (a_message != WM_KEYDOWN && a_message != WM_SYSKEYDOWN) {
				flags |= RI_KEY_BREAK;
			}

			std::uint32_t dik{};
			if (a_wParam == VK_PAUSE) {
				dik = REX::W32::DIK_PAUSE;
			} else if (a_wParam == VK_SNAPSHOT) {
				dik = REX::W32::DIK_SYSRQ;
			}
			return DecodeKeyboard(
				makeCode, flags, static_cast<USHORT>(a_wParam), dik);
		}

		[[nodiscard]] bool ApplyMainWindowKeyboardEdge(
			bool          a_open,
			std::uint32_t a_dik,
			std::int32_t  a_expectedEngineEventID,
			InputCapture::KeyboardEdgeMatch a_match =
				InputCapture::KeyboardEdgeMatch::InitialPress) noexcept
		{
			if (!WindowManager::SetMainWindowOpen(a_open)) {
				return false;
			}

			const bool suppressionQueued =
				InputCapture::RequestKeyboardSuppression(
					a_expectedEngineEventID,
					a_match);

			logger::info(
				"Raw DIK {} {} the Mod Control Panel; native batch suppression {}",
				a_dik,
				a_open ? "opened" : "closed",
				suppressionQueued ? "queued" : "unavailable");
			return true;
		}

		[[nodiscard]] UINT_PTR NextKeyboardHoldTimerID() noexcept
		{
			auto generation =
				keyboardHoldTimerGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
			while (generation == 0) {
				generation =
					keyboardHoldTimerGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
			}
			return keyboardHoldTimerTag | static_cast<UINT_PTR>(generation);
		}

		[[nodiscard]] bool IsKeyboardHoldTimerID(UINT_PTR a_timerID) noexcept
		{
			return (a_timerID & keyboardHoldTimerTagMask) == keyboardHoldTimerTag;
		}

		[[nodiscard]] bool ArmKeyboardHold(
			HWND          a_window,
			std::uint32_t a_dik,
			std::int32_t  a_expectedEngineEventID) noexcept
		{
			CancelKeyboardHold();
			auto& state = State<KeyboardToggleState>();
			const auto timerID = NextKeyboardHoldTimerID();
			state.HoldDIK = a_dik;
			state.HoldEngineEventID = a_expectedEngineEventID;
			state.HoldWindow = a_window;
			const auto installedTimerID = ::SetTimer(
				a_window,
				timerID,
				keyboardHoldThresholdMilliseconds,
				nullptr);
			if (installedTimerID != timerID) {
				if (installedTimerID != 0) {
					static_cast<void>(::KillTimer(a_window, installedTimerID));
				}
				CancelKeyboardHold();
				if (FirstFailure(keyboardHoldTimerFailureLogged)) {
					logger::warn("Failed to arm the configured keyboard hold timer");
				}
				return false;
			}
			state.HoldTimerID = installedTimerID;
			return true;
		}
	}

	bool Detail::ProcessKeyboardHoldTimer(
		HWND a_window,
		UINT_PTR a_timerID) noexcept
	{
		if (!IsKeyboardHoldTimerID(a_timerID)) {
			return false;
		}

		auto& state = State<KeyboardToggleState>();
		if (state.HoldTimerID != a_timerID || state.HoldWindow != a_window) {
			// A killed timer message may already be queued. Its generation-tagged
			// ID cannot act on a newer hold.
			return true;
		}
		static_cast<void>(::KillTimer(a_window, a_timerID));
		state.HoldTimerID = 0;

		const auto dik = state.HoldDIK;
		const auto expectedEngineEventID = state.HoldEngineEventID;
		const auto* mainWindow = WindowManager::GetMainWindow();
		const bool valid =
			dik != 0 && dik < state.Down.size() && state.Down[dik] &&
			::GetForegroundWindow() == a_window &&
			InputCapture::IsOperational() &&
			WindowManager::IsHotkeyEnabled() && mainWindow &&
			!mainWindow->IsOpen.load(std::memory_order_acquire) &&
			FrameworkSettings::GetToggleMode() ==
				FrameworkSettings::ToggleMode::Hold &&
			FrameworkSettings::GetToggleKey() == dik;
		if (!valid) {
			CancelKeyboardHold();
			return true;
		}

		if (!ApplyMainWindowKeyboardEdge(
				true,
				dik,
				expectedEngineEventID,
				InputCapture::KeyboardEdgeMatch::HeldPress)) {
			CancelKeyboardHold();
		}
		return true;
	}

	namespace
	{
		[[nodiscard]] bool ProcessKeyboardTransition(
			HWND                      a_window,
			const KeyboardTransition& a_transition) noexcept
		{
			auto& state = State<KeyboardToggleState>();
			const auto dik = a_transition.DIK;
			if (dik >= state.Down.size() || ::GetForegroundWindow() != a_window) {
				return false;
			}
			const bool wasDown = state.Down[dik];
			state.Down[dik] = a_transition.Down;
			if (!a_transition.Down) {
				if (state.HoldDIK == dik) {
					CancelKeyboardHold();
				}
				return false;
			}
			if (wasDown || !InputCapture::IsOperational()) {
				return false;
			}

			const auto* mainWindow = WindowManager::GetMainWindow();
			if (!mainWindow) {
				return false;
			}
			const bool mainWindowOpen =
				mainWindow->IsOpen.load(std::memory_order_acquire);
			const auto closeMainWindow = [&]() {
				CancelKeyboardHold();
				return ApplyMainWindowKeyboardEdge(
					false, dik, a_transition.EngineEventID);
			};
			if (dik == REX::W32::DIK_ESCAPE && mainWindowOpen) {
				return closeMainWindow();
			}

			const auto configured = FrameworkSettings::GetToggleKey();
			if (!WindowManager::IsHotkeyEnabled() || dik != configured) {
				return false;
			}

			if (dik == REX::W32::DIK_F4 &&
				(state.Down[REX::W32::DIK_LMENU] ||
				 state.Down[REX::W32::DIK_RMENU])) {
				return false;
			}

			if (mainWindowOpen) {
				return closeMainWindow();
			}

			switch (FrameworkSettings::GetToggleMode()) {
			case FrameworkSettings::ToggleMode::SinglePress:
				return ApplyMainWindowKeyboardEdge(
					true,
					dik,
					a_transition.EngineEventID);
			case FrameworkSettings::ToggleMode::Hold:
				return ArmKeyboardHold(
					a_window,
					dik,
					a_transition.EngineEventID);
			case FrameworkSettings::ToggleMode::DoublePress: {
				const auto now = std::chrono::steady_clock::now();
				const bool doublePress =
					state.HasLastPress && state.LastPressDIK == dik &&
					now - state.LastPress < doublePressThreshold;
				state.LastPress = now;
				state.LastPressDIK = dik;
				state.HasLastPress = !doublePress;
				return doublePress && ApplyMainWindowKeyboardEdge(
					true,
					dik,
					a_transition.EngineEventID);
			}
			case FrameworkSettings::ToggleMode::Off:
			default:
				return false;
			}
		}
	}

	void Detail::ProcessRawKeyboard(
		HWND a_window, const RAWKEYBOARD& a_keyboard) noexcept
	{
		if (a_keyboard.MakeCode == 0 ||
			a_keyboard.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE || a_keyboard.VKey >= 0xFF ||
			(a_keyboard.Flags & (RI_KEY_E0 | RI_KEY_E1)) == (RI_KEY_E0 | RI_KEY_E1)) {
			return;
		}
		if (const auto transition = DecodeKeyboard(
				a_keyboard.MakeCode, a_keyboard.Flags, a_keyboard.VKey)) {
			static_cast<void>(
				ProcessKeyboardTransition(a_window, *transition));
		}
	}

	void Detail::ProcessLegacyKeyboard(
		HWND a_window, UINT a_message, WPARAM a_wParam,
		LPARAM a_lParam) noexcept
	{
		if (const auto transition =
				ReadLegacyKeyboard(a_message, a_wParam, a_lParam)) {
			static_cast<void>(
				ProcessKeyboardTransition(a_window, *transition));
		}
	}
}
