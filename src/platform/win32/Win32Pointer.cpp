#include "platform/win32/Win32PlatformInternal.h"

#include "runtime/WindowManager.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace SFSEMenuFramework::Win32Platform
{
	using namespace Detail;

	namespace
	{
		struct RawMouseBatch final
		{
			std::array<QueuedWindowMessage, rawMouseMessageCapacity> Messages{};
			std::size_t Count{};

			void Push(HWND a_window, UINT a_message, WPARAM a_wParam, LPARAM a_lParam) noexcept
			{
				if (Count < Messages.size()) {
					Messages[Count++] = { a_window, a_message, a_wParam, a_lParam };
				}
			}
		};
		struct WindowThreadMouseState final
		{
			std::uint32_t ButtonsDown{ 0 };
			int           TrackedArea{ 0 };
			bool          ReleasingCapture{ false };
		};

		struct RawMouseState final
		{
			HWND          Window{ nullptr };
			std::uint64_t Generation{ 0 };
			std::int64_t  X{ 0 };
			std::int64_t  Y{ 0 };
			std::uint32_t ButtonsDown{ 0 };
			bool          Initialized{ false };
		};

		struct RawButtonBinding final
		{
			USHORT DownFlag;
			USHORT UpFlag;
			UINT   DownMessage;
			UINT   UpMessage;
			WORD   XButton;
		};

		constexpr std::array<RawButtonBinding, 5> rawButtonBindings{ {
			{ RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, WM_LBUTTONDOWN, WM_LBUTTONUP, 0 },
			{ RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, WM_RBUTTONDOWN, WM_RBUTTONUP, 0 },
			{ RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, WM_MBUTTONDOWN, WM_MBUTTONUP, 0 },
			{ RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON1 },
			{ RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON2 }
		} };
		std::atomic_flag                mouseCaptureFailureLogged{};
		std::atomic_flag                mouseTrackingFailureLogged{};
		std::atomic_flag                rawMouseHandoffFailureLogged{};

		template <class T>
		[[nodiscard]] T& State()
		{
			static auto* state = new T();
			return *state;
		}

		[[nodiscard]] bool SupportsRawMouseCoordinates(const RECT& a_area) noexcept
		{
			return a_area.left >= 0 && a_area.top >= 0 &&
			       a_area.right - 1 <= (std::numeric_limits<SHORT>::max)() &&
			       a_area.bottom - 1 <= (std::numeric_limits<SHORT>::max)();
		}

		[[nodiscard]] POINT ClampClientPosition(
			std::int64_t a_x, std::int64_t a_y, const RECT& a_area) noexcept
		{
			return {
				static_cast<LONG>(std::clamp<std::int64_t>(a_x, a_area.left, a_area.right - 1)),
				static_cast<LONG>(std::clamp<std::int64_t>(a_y, a_area.top, a_area.bottom - 1))
			};
		}
	}

	void Detail::ResetRawMouseState() noexcept
	{
		State<RawMouseState>() = {};
	}

	void Detail::DeactivateRawMouseState() noexcept
	{
		auto& state = State<RawMouseState>();
		state.Generation = 0;
		state.ButtonsDown = 0;
		state.Initialized = false;
	}

	namespace
	{
		[[nodiscard]] std::uint32_t MouseButtonMask(
			UINT a_message,
			WPARAM a_wParam) noexcept
		{
			if (a_message >= WM_LBUTTONDOWN && a_message <= WM_LBUTTONDBLCLK) {
				return 1U << 0;
			}
			if (a_message >= WM_RBUTTONDOWN && a_message <= WM_RBUTTONDBLCLK) {
				return 1U << 1;
			}
			if (a_message >= WM_MBUTTONDOWN && a_message <= WM_MBUTTONDBLCLK) {
				return 1U << 2;
			}
			if (a_message >= WM_XBUTTONDOWN && a_message <= WM_XBUTTONDBLCLK) {
				return HIWORD(a_wParam) == XBUTTON1 ? 1U << 3 : 1U << 4;
			}
			return 0;
		}

		[[nodiscard]] bool IsMouseButtonDown(UINT a_message) noexcept
		{
			return a_message == WM_LBUTTONDOWN || a_message == WM_LBUTTONDBLCLK ||
			       a_message == WM_RBUTTONDOWN || a_message == WM_RBUTTONDBLCLK ||
			       a_message == WM_MBUTTONDOWN || a_message == WM_MBUTTONDBLCLK ||
			       a_message == WM_XBUTTONDOWN || a_message == WM_XBUTTONDBLCLK;
		}

		[[nodiscard]] bool IsMouseButtonUp(UINT a_message) noexcept
		{
			return a_message == WM_LBUTTONUP || a_message == WM_RBUTTONUP ||
			       a_message == WM_MBUTTONUP || a_message == WM_XBUTTONUP;
		}

		void CancelMouseTracking(HWND a_window, WindowThreadMouseState& a_state)
		{
			if (a_state.TrackedArea == 0) {
				return;
			}

			TRACKMOUSEEVENT event{
				.cbSize = sizeof(event),
				.dwFlags = TME_CANCEL,
				.hwndTrack = a_window,
				.dwHoverTime = 0
			};
			static_cast<void>(::TrackMouseEvent(&event));
			a_state.TrackedArea = 0;
		}

		void TrackMouseArea(HWND a_window, int a_area)
		{
			auto& state = State<WindowThreadMouseState>();
			if (state.TrackedArea == a_area) {
				return;
			}

			CancelMouseTracking(a_window, state);
			TRACKMOUSEEVENT event{
				.cbSize = sizeof(event),
				.dwFlags = static_cast<DWORD>(
					TME_LEAVE | (a_area == 2 ? TME_NONCLIENT : 0)),
				.hwndTrack = a_window,
				.dwHoverTime = 0
			};
			if (::TrackMouseEvent(&event)) {
				state.TrackedArea = a_area;
			} else if (FirstFailure(mouseTrackingFailureLogged)) {
				logger::warn("Starfield HWND mouse tracking could not be armed");
			}
		}
	}

	void Detail::ResetWindowThreadMouseState(HWND a_window)
	{
		auto& state = State<WindowThreadMouseState>();
		CancelMouseTracking(a_window, state);
		state.ButtonsDown = 0;
		if (::GetCapture() == a_window) {
			static_cast<void>(::ReleaseCapture());
		}
	}

	void Detail::UpdateWindowThreadMouseState(
		HWND a_window,
		UINT a_message,
		WPARAM a_wParam)
	{
		auto& state = State<WindowThreadMouseState>();
		if (a_message == WM_MOUSEMOVE) {
			TrackMouseArea(a_window, 1);
		} else if (a_message == WM_NCMOUSEMOVE) {
			TrackMouseArea(a_window, 2);
		} else if ((a_message == WM_MOUSELEAVE && state.TrackedArea == 1) ||
				   (a_message == WM_NCMOUSELEAVE && state.TrackedArea == 2)) {
			state.TrackedArea = 0;
		}

		const auto buttonMask = MouseButtonMask(a_message, a_wParam);
		if (buttonMask == 0) {
			return;
		}

		if (IsMouseButtonDown(a_message)) {
			if (state.ButtonsDown == 0 && ::GetCapture() == nullptr) {
				static_cast<void>(::SetCapture(a_window));
				if (::GetCapture() != a_window &&
					FirstFailure(mouseCaptureFailureLogged)) {
					logger::warn("Starfield HWND mouse capture could not be acquired");
				}
			}
			state.ButtonsDown |= buttonMask;
			return;
		}

		if (IsMouseButtonUp(a_message)) {
			state.ButtonsDown &= ~buttonMask;
			if (state.ButtonsDown == 0 && ::GetCapture() == a_window) {
				state.ReleasingCapture = true;
				static_cast<void>(::ReleaseCapture());
				state.ReleasingCapture = false;
			}
		}
	}

	namespace
	{
		[[nodiscard]] WORD RawMouseKeyState(
			std::uint32_t a_buttonsDown) noexcept
		{
			constexpr std::array masks{
				MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2
			};
			WORD result{};
			for (std::size_t index = 0; index < masks.size(); ++index) {
				if ((a_buttonsDown & (1U << index)) != 0) {
					result |= masks[index];
				}
			}
			return result;
		}

		// Starfield-specific Windows Raw Input bridge. SKSE Menu Framework 3 at
		// 928e01ab459822a8d233ab99f0419ea1de23c775 has no relative-motion path;
		// its cursor position is supplied by the stock Dear ImGui Win32 backend.

		[[nodiscard]] LPARAM RawMousePositionParameter(
			const RawMouseState& a_state) noexcept
		{
			return MAKELPARAM(
				static_cast<WORD>(a_state.X),
				static_cast<WORD>(a_state.Y));
		}

		[[nodiscard]] bool UpdateRawMousePosition(
			HWND a_window,
			const RAWMOUSE& a_mouse,
			RawMouseState& a_state,
			bool& a_positionChanged) noexcept
		{
			RECT clientArea{};
			if (!ReadClientArea(a_window, clientArea) ||
				!SupportsRawMouseCoordinates(clientArea)) {
				return false;
			}

			const auto previousX = a_state.X;
			const auto previousY = a_state.Y;
			if ((a_mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
				const bool virtualDesktop =
					(a_mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
				const int originX = virtualDesktop ?
					::GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
				const int originY = virtualDesktop ?
					::GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
				const int width = ::GetSystemMetrics(
					virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
				const int height = ::GetSystemMetrics(
					virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
				if (width <= 0 || height <= 0) {
					return false;
				}

				const auto normalizedX = static_cast<int>(std::clamp<std::int64_t>(
					a_mouse.lLastX,
					0,
					65535));
				const auto normalizedY = static_cast<int>(std::clamp<std::int64_t>(
					a_mouse.lLastY,
					0,
					65535));
				POINT point{
					originX + ::MulDiv(normalizedX, width - 1, 65535),
					originY + ::MulDiv(normalizedY, height - 1, 65535)
				};
				if (!::ScreenToClient(a_window, &point)) {
					return false;
				}
				a_state.X = point.x;
				a_state.Y = point.y;
			} else {
				a_state.X += a_mouse.lLastX;
				a_state.Y += a_mouse.lLastY;
			}

			const auto position = ClampClientPosition(a_state.X, a_state.Y, clientArea);
			a_state.X = position.x;
			a_state.Y = position.y;
			a_positionChanged =
				a_state.X != previousX || a_state.Y != previousY;
			return true;
		}

		void AppendRawButtonTransition(
			RawMouseBatch& a_batch,
			HWND a_window,
			RawMouseState& a_state,
			USHORT a_rawFlags,
			std::uint32_t a_buttonMask,
			const RawButtonBinding& a_binding) noexcept
		{
			const auto position = RawMousePositionParameter(a_state);
			if ((a_rawFlags & a_binding.DownFlag) != 0) {
				a_state.ButtonsDown |= a_buttonMask;
				const auto keys = RawMouseKeyState(a_state.ButtonsDown);
				a_batch.Push(a_window, a_binding.DownMessage,
					a_binding.XButton ? MAKEWPARAM(keys, a_binding.XButton) : keys,
					position);
			}
			if ((a_rawFlags & a_binding.UpFlag) != 0) {
				a_state.ButtonsDown &= ~a_buttonMask;
				const auto keys = RawMouseKeyState(a_state.ButtonsDown);
				a_batch.Push(a_window, a_binding.UpMessage,
					a_binding.XButton ? MAKEWPARAM(keys, a_binding.XButton) : keys,
					position);
			}
		}
	}

	bool Detail::ProcessEarlyRawMouse(
		HWND a_window,
		const RAWMOUSE& a_mouse) noexcept
	{
		auto& state = State<RawMouseState>();
		const auto generation =
			Shared().EarlyRawMouseGeneration.load(std::memory_order_acquire);
		if (!state.Initialized || state.Window != a_window || generation == 0 ||
			state.Generation != generation ||
			!WindowManager::IsBlockingWindowOpenGeneration(generation)) {
			return false;
		}

		bool positionChanged{};
		if (!UpdateRawMousePosition(
				a_window,
				a_mouse,
				state,
				positionChanged)) {
			return false;
		}

		RawMouseBatch batch;
		const bool hasButtonsOrWheel = a_mouse.usButtonFlags != 0;
		if (positionChanged || hasButtonsOrWheel) {
			batch.Push(a_window, WM_MOUSEMOVE,
				RawMouseKeyState(state.ButtonsDown), RawMousePositionParameter(state));
		}

		for (std::size_t index = 0; index < rawButtonBindings.size(); ++index) {
			AppendRawButtonTransition(
				batch, a_window, state, a_mouse.usButtonFlags,
				1U << index, rawButtonBindings[index]);
		}

		const auto keys = RawMouseKeyState(state.ButtonsDown);
		if ((a_mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
			batch.Push(a_window, WM_MOUSEWHEEL,
				MAKEWPARAM(keys, a_mouse.usButtonData), RawMousePositionParameter(state));
		}
		if ((a_mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0) {
			batch.Push(a_window, WM_MOUSEHWHEEL,
				MAKEWPARAM(keys, a_mouse.usButtonData), RawMousePositionParameter(state));
		}

		if (batch.Count == 0) {
			return true;
		}
		for (std::size_t index = 0; index < batch.Count; ++index) {
			UpdateWindowThreadMouseState(
				a_window,
				batch.Messages[index].Message,
				batch.Messages[index].WParam);
		}
		if (!EnqueueRawMouseBatch(batch.Messages, batch.Count, generation)) {
			state.ButtonsDown = 0;
			ResetWindowThreadMouseState(a_window);
			return false;
		}
		return true;
	}

	bool Detail::InitializeEarlyRawMouse(
		HWND a_window,
		std::uint64_t a_generation,
		QueuedWindowMessage& a_seedMessage) noexcept
	{
		if (!a_window || a_generation == 0 ||
			!WindowManager::IsBlockingWindowOpenGeneration(a_generation)) {
			return false;
		}

		RECT clientArea{};
		if (!ReadClientArea(a_window, clientArea) ||
			!SupportsRawMouseCoordinates(clientArea)) {
			return false;
		}

		ResetWindowThreadMouseState(a_window);
		auto& state = State<RawMouseState>();
		std::int64_t initialX{};
		std::int64_t initialY{};
		if (state.Window == a_window) {
			initialX = state.X;
			initialY = state.Y;
		} else {
			POINT cursorPosition{};
			if (::GetCursorPos(&cursorPosition) &&
				::ScreenToClient(a_window, &cursorPosition)) {
				initialX = cursorPosition.x;
				initialY = cursorPosition.y;
			} else {
				// A bounded virtual starting point is still required when
				// Windows cannot expose the first absolute pointer position.
				// This fallback does not move the OS cursor.
				initialX = clientArea.left +
					(clientArea.right - clientArea.left) / 2;
				initialY = clientArea.top +
					(clientArea.bottom - clientArea.top) / 2;
			}
		}
		const auto position = ClampClientPosition(initialX, initialY, clientArea);
		state = {
			.Window = a_window,
			.Generation = a_generation,
			.X = position.x,
			.Y = position.y,
			.ButtonsDown = 0,
			.Initialized = true
		};
		a_seedMessage = {
			.Window = a_window,
			.Message = WM_MOUSEMOVE,
			.WParam = 0,
			.LParam = RawMousePositionParameter(state)
		};
		return true;
	}

	bool Detail::PlaceLegacyCursorForRawHandoff(
		HWND a_window) noexcept
	{
		RECT clientArea{};
		if (!ReadClientArea(a_window, clientArea)) {
			return false;
		}

		POINT clientPosition{};
		const auto& state = State<RawMouseState>();
		if (state.Initialized && state.Window == a_window) {
			clientPosition = ClampClientPosition(state.X, state.Y, clientArea);
		} else {
			clientPosition = {
				clientArea.left +
					(clientArea.right - clientArea.left) / 2,
				clientArea.top +
					(clientArea.bottom - clientArea.top) / 2
			};
		}

		auto screenPosition = clientPosition;
		if (::ClientToScreen(a_window, &screenPosition) &&
			::SetCursorPos(screenPosition.x, screenPosition.y)) {
			return true;
		}

		if (FirstFailure(rawMouseHandoffFailureLogged)) {
			logger::critical(
				"Early raw mouse position could not be handed off to the Starfield cursor");
		}
		return false;
	}

	void Detail::ResetCapturedMouse(HWND a_window)
	{
		State<RawMouseState>().ButtonsDown = 0;
		ResetWindowThreadMouseState(a_window);
		RequestInputReset();
	}

	void Detail::HandleCaptureChanged(HWND a_window)
	{
		auto& state = State<WindowThreadMouseState>();

		// ReleaseCapture() normally emits WM_CAPTURECHANGED after the final
		// mouse-button-up. Preserve that release event so ImGui MenuItem and
		// Selectable widgets can activate.
		if (state.ReleasingCapture) {
			state.ReleasingCapture = false;
			return;
		}

		ResetCapturedMouse(a_window);
	}

	bool Detail::IsEarlyRawMouseReady(
		HWND a_window, std::uint64_t a_generation) noexcept
	{
		const auto& state = State<RawMouseState>();
		return state.Initialized && state.Window == a_window &&
			state.Generation == a_generation;
	}
}
