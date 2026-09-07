#include "platform/win32/Win32Platform.h"
#include "platform/win32/Win32PlatformInternal.h"

#include "input/BindingCapture.h"
#include "input/GamepadNavigation.h"
#include "input/InputCapture.h"
#include "rendering/D3D12Renderer.h"
#include "runtime/WindowManager.h"

#include <CommCtrl.h>
#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace SFSEMenuFramework::Win32Platform
{
	using namespace Detail;

	namespace Detail
	{
		namespace
		{
			SharedState sharedState;
		}

		SharedState& Shared() noexcept
		{
			return sharedState;
		}
	}

	namespace
	{
		struct WindowSearch final
		{
			HWND        Window{ nullptr };
			DWORD       ThreadID{ 0 };
			std::size_t CandidateCount{ 0 };
		};

		std::atomic_flag callbackPostFailureLogged{};
		std::atomic_flag subclassDriftLogged{};
		std::atomic_flag ambiguousWindowLogged{};
		std::atomic_flag subclassInstallFailureLogged{};
		std::atomic_flag subclassVerificationFailureLogged{};

		LRESULT CALLBACK WindowSubclass(
			HWND a_window, UINT a_message, WPARAM a_wParam,
			LPARAM a_lParam, UINT_PTR a_subclassID,
			DWORD_PTR a_referenceData);

		[[nodiscard]] UINT_PTR SubclassID() noexcept
		{
			return reinterpret_cast<UINT_PTR>(&WindowSubclass);
		}

		[[nodiscard]] bool RawInputReadFailed() noexcept
		{
			static std::atomic_flag logged{};
			if (FirstFailure(logged)) {
				logger::warn("Starfield raw input packet could not be read");
			}
			return false;
		}

		[[nodiscard]] bool ReadRawInputHeader(LPARAM a_lParam, RAWINPUTHEADER& a_header) noexcept
		{
			UINT size = sizeof(a_header);
			const auto copied = ::GetRawInputData(
				reinterpret_cast<HRAWINPUT>(a_lParam), RID_HEADER,
				&a_header, &size, sizeof(a_header));
			if (copied != sizeof(a_header)) {
				return RawInputReadFailed();
			}
			return true;
		}

		// Called only for a keyboard packet or a mouse packet owned by the early route.
		[[nodiscard]] bool ReadRawInputPayload(
			LPARAM a_lParam, const RAWINPUTHEADER& a_header, RAWINPUT& a_input) noexcept
		{
			const auto payloadSize = a_header.dwType == RIM_TYPEKEYBOARD ?
				sizeof(RAWKEYBOARD) : sizeof(RAWMOUSE);
			UINT size = sizeof(a_input);
			const auto copied = ::GetRawInputData(reinterpret_cast<HRAWINPUT>(a_lParam), RID_INPUT,
				&a_input, &size, sizeof(a_header));
			if (copied == static_cast<UINT>(-1) || copied < sizeof(a_header) + payloadSize ||
				a_input.header.dwType != a_header.dwType) {
				return RawInputReadFailed();
			}
			return true;
		}
	}

	bool Detail::ReadClientArea(HWND a_window, RECT& a_area) noexcept
	{
		return a_window && ::GetClientRect(a_window, &a_area) &&
		       a_area.right > a_area.left && a_area.bottom > a_area.top;
	}

	bool Detail::HasCurrentInputLease() noexcept
	{
		const auto generation = WindowManager::GetBlockingWindowOpenGeneration();
		return D3D12Renderer::HasRecentBlockingWindowFrame(generation) &&
		       WindowManager::IsBlockingWindowOpenGeneration(generation);
	}

	namespace
	{
		[[nodiscard]] UINT HostWindowCallbackMessage() noexcept
		{
			static const auto message =
				::RegisterWindowMessageW(L"SFSEMenuFramework.HostWindowCallback");
			return message;
		}
		[[nodiscard]] bool IsCandidate(HWND a_window, RECT& a_clientArea, DWORD& a_threadID)
		{
			if (!a_window || !::IsWindowVisible(a_window) ||
				::GetWindow(a_window, GW_OWNER) ||
				::GetAncestor(a_window, GA_ROOT) != a_window) {
				return false;
			}

			DWORD processID{};
			a_threadID = ::GetWindowThreadProcessId(a_window, &processID);
			if (!a_threadID || processID != ::GetCurrentProcessId() ||
				!ReadClientArea(a_window, a_clientArea)) {
				return false;
			}
			return true;
		}

		BOOL CALLBACK FindWindowCallback(HWND a_window, LPARAM a_parameter)
		{
			auto& search = *reinterpret_cast<WindowSearch*>(a_parameter);
			RECT clientArea{};
			DWORD threadID{};
			if (!IsCandidate(a_window, clientArea, threadID)) {
				return TRUE;
			}

			++search.CandidateCount;
			if (search.CandidateCount == 1) {
				search.Window = a_window;
				search.ThreadID = threadID;
			}
			return TRUE;
		}

		[[nodiscard]] WindowSearch FindHostWindow()
		{
			WindowSearch search;
			::EnumWindows(&FindWindowCallback, reinterpret_cast<LPARAM>(&search));
			return search;
		}
		[[nodiscard]] bool IsAltF4(
			UINT a_message,
			WPARAM a_wParam,
			LPARAM a_lParam) noexcept
		{
			if (!IsKeyMessage(a_message) || a_wParam != VK_F4) {
				return false;
			}
			const auto bits = static_cast<std::uintptr_t>(a_lParam);
			return a_message == WM_SYSKEYDOWN || a_message == WM_SYSKEYUP ||
			       (bits & (std::uintptr_t{ 1 } << 29)) != 0;
		}

		[[nodiscard]] bool IsScreenshotMessage(UINT a_message, WPARAM a_wParam) noexcept
		{
			return IsKeyMessage(a_message) && a_wParam == VK_SNAPSHOT;
		}

		[[nodiscard]] bool IsFocusOrActivationMessage(UINT a_message) noexcept
		{
			return a_message == WM_SETFOCUS || a_message == WM_KILLFOCUS ||
			       a_message == WM_ACTIVATE || a_message == WM_ACTIVATEAPP;
		}

		[[nodiscard]] bool IsLegacyMouseMessage(UINT a_message) noexcept
		{
			return (a_message >= WM_MOUSEMOVE && a_message <= WM_MOUSEHWHEEL) ||
			       a_message == WM_NCMOUSEMOVE || a_message == WM_MOUSELEAVE ||
			       a_message == WM_NCMOUSELEAVE;
		}

		[[nodiscard]] bool IsBackendInputMessage(UINT a_message) noexcept
		{
			return IsLegacyMouseMessage(a_message) || IsKeyMessage(a_message) ||
			       a_message == WM_CHAR || a_message == WM_SETFOCUS ||
			       a_message == WM_KILLFOCUS || a_message == WM_INPUTLANGCHANGE;
		}

		[[nodiscard]] bool ShouldConsumeModalMessage(
			UINT a_message,
			WPARAM a_wParam,
			LPARAM a_lParam) noexcept
		{
			if (IsAltF4(a_message, a_wParam, a_lParam) ||
				IsScreenshotMessage(a_message, a_wParam) ||
				IsFocusOrActivationMessage(a_message) ||
				a_message == WM_SYSKEYDOWN || a_message == WM_SYSKEYUP) {
				return false;
			}

			return a_message != WM_MOUSELEAVE &&
			       a_message != WM_NCMOUSELEAVE &&
			       a_message != WM_INPUTLANGCHANGE &&
			       (IsLegacyMouseMessage(a_message) || IsKeyMessage(a_message) ||
			        a_message == WM_CHAR);
		}

		[[nodiscard]] bool VerifySubclass(HWND a_window)
		{
			DWORD_PTR referenceData{};
			return ::GetWindowSubclass(
					   a_window,
					   &WindowSubclass,
					   SubclassID(),
					   &referenceData) != FALSE &&
			       referenceData == 0;
		}

		void InvalidateHostWindow()
		{
			static_cast<void>(UpdateInputState(false));
			ResetKeyboardToggleState();
			ResetRawMouseState();
			Shared().HostWindowTearingDown.store(true, std::memory_order_release);
			Shared().HostCallbackPending.store(false, std::memory_order_release);
			if (const auto callback = Shared().HostWindowCallbackFunction.load(std::memory_order_acquire)) {
				callback();
			}
			Shared().SubclassActive.store(false, std::memory_order_release);
			Shared().InitializedHostWindow.store(nullptr, std::memory_order_relaxed);
			Shared().InitializedHostWindowThreadID.store(0, std::memory_order_release);
			RequestInputReset();
		}

		LRESULT CALLBACK WindowSubclass(
			HWND      a_window,
			UINT      a_message,
			WPARAM    a_wParam,
			LPARAM    a_lParam,
			UINT_PTR  a_subclassID,
			DWORD_PTR)
		{
			if (a_subclassID != SubclassID() ||
				!Shared().SubclassActive.load(std::memory_order_acquire)) {
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			const auto callbackMessage = HostWindowCallbackMessage();
			if (callbackMessage != 0 && a_message == callbackMessage) {
				const bool wasPending =
					Shared().HostCallbackPending.exchange(false, std::memory_order_acq_rel);
				if (wasPending) {
					if (const auto callback =
							Shared().HostWindowCallbackFunction.load(std::memory_order_acquire)) {
						callback();
					}
				}
				return 0;
			}

			if (a_message == WM_TIMER &&
				ProcessKeyboardHoldTimer(a_window, a_wParam)) {
				return 0;
			}

			if (a_message == WM_INPUT) {
				RAWINPUTHEADER header{};
				const bool headerValid = ReadRawInputHeader(a_lParam, header);
				RAWINPUT input{};
				const bool payloadValid =
					headerValid &&
					(header.dwType == RIM_TYPEKEYBOARD ||
						header.dwType == RIM_TYPEMOUSE) &&
					ReadRawInputPayload(a_lParam, header, input);
				if (payloadValid && header.dwType == RIM_TYPEKEYBOARD) {
					ProcessRawKeyboard(a_window, input.data.keyboard);
				}
				if (payloadValid && header.dwType == RIM_TYPEMOUSE &&
					(input.data.mouse.lLastX != 0 ||
					 input.data.mouse.lLastY != 0 ||
					 input.data.mouse.usButtonFlags != 0)) {
					BindingCapture::ObserveMouseActivity();
				}
				const bool acceptingRawInput =
					Shared().AcceptInput.load(std::memory_order_acquire);
				const bool currentInputLease =
					acceptingRawInput && HasCurrentInputLease();
				if (acceptingRawInput && !currentInputLease) {
					static_cast<void>(UpdateInputState(false));
					static_cast<void>(PostHostWindowCallback());
					return ::DefSubclassProc(
						a_window,
						a_message,
						a_wParam,
						a_lParam);
				}
				if (currentInputLease && payloadValid &&
					header.dwType == RIM_TYPEMOUSE &&
					(input.data.mouse.lLastX != 0 ||
						input.data.mouse.lLastY != 0 ||
						input.data.mouse.usButtonFlags != 0)) {
					GamepadNavigation::ObserveMouseActivity(
						WindowManager::GetBlockingWindowOpenGeneration());
				}
				const bool earlyRawInput =
					Shared().PointerRouteState.load(std::memory_order_acquire) ==
						PointerRoute::EarlyRaw &&
					currentInputLease;
				if (earlyRawInput) {
					const auto generation =
						Shared().EarlyRawMouseGeneration.load(std::memory_order_acquire);
					const bool failed =
						!headerValid ||
						(header.dwType == RIM_TYPEMOUSE &&
						 (!payloadValid ||
						  !ProcessEarlyRawMouse(a_window, input.data.mouse)));
					if (failed) {
						if (generation != 0) {
							Shared().RawMouseFaultGeneration.store(
								generation,
								std::memory_order_release);
						}
						logger::critical(
							"Early raw mouse input failed for generation {}; closing all blocking framework windows",
							generation);
						static_cast<void>(UpdateInputState(false));
						WindowManager::CloseAllBlockingWindows();
						InputCapture::SetModal(false);
						static_cast<void>(PostHostWindowCallback());
					}
				}
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			if (a_message == WM_NCDESTROY) {
				InvalidateHostWindow();
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			if (a_message == WM_CAPTURECHANGED) {
				HandleCaptureChanged(a_window);
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			if (a_message == WM_CANCELMODE) {
				ResetCapturedMouse(a_window);
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			if (IsFocusOrActivationMessage(a_message)) {
				const bool losingFocus =
					a_message == WM_KILLFOCUS ||
					(a_message == WM_ACTIVATEAPP && a_wParam == FALSE) ||
					(a_message == WM_ACTIVATE && LOWORD(a_wParam) == WA_INACTIVE);
				if (losingFocus) {
					static_cast<void>(UpdateInputState(false));
					ResetKeyboardToggleState();
				}
				if (a_message == WM_SETFOCUS || a_message == WM_KILLFOCUS) {
					static_cast<void>(EnqueueWindowMessage(
						a_window,
						a_message,
						a_wParam,
						a_lParam,
						false));
				}
				static_cast<void>(PostHostWindowCallback());
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			if (a_message == WM_INPUTLANGCHANGE) {
				ResetKeyboardToggleState();
				static_cast<void>(EnqueueWindowMessage(
					a_window,
					a_message,
					a_wParam,
					a_lParam,
					false));
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			if (IsKeyMessage(a_message) &&
				!IsAltF4(a_message, a_wParam, a_lParam)) {
				ProcessLegacyKeyboard(
					a_window, a_message, a_wParam, a_lParam);
			}
			if (IsLegacyMouseMessage(a_message) &&
				a_message != WM_MOUSELEAVE && a_message != WM_NCMOUSELEAVE) {
				BindingCapture::ObserveMouseActivity();
			}

			const bool modalInput =
				Shared().AcceptInput.load(std::memory_order_acquire) && HasCurrentInputLease();
			if (Shared().AcceptInput.load(std::memory_order_relaxed) && !modalInput) {
				static_cast<void>(UpdateInputState(false));
				static_cast<void>(PostHostWindowCallback());
			}

			if (!modalInput || !IsBackendInputMessage(a_message) ||
				IsAltF4(a_message, a_wParam, a_lParam)) {
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			const bool suppressDuplicateLegacyMouse =
				IsLegacyMouseMessage(a_message) &&
				Shared().PointerRouteState.load(std::memory_order_acquire) ==
					PointerRoute::EarlyRaw;
			const bool suppressCapturedKeyboard =
				IsKeyMessage(a_message) &&
				!BindingCapture::ShouldForwardKeyboardMessage(
					static_cast<std::uint32_t>(a_wParam),
					a_message == WM_KEYDOWN || a_message == WM_SYSKEYDOWN);
			if (IsLegacyMouseMessage(a_message) &&
				!suppressDuplicateLegacyMouse) {
				UpdateWindowThreadMouseState(a_window, a_message, a_wParam);
			}
			if (!suppressDuplicateLegacyMouse && !suppressCapturedKeyboard) {
				static_cast<void>(EnqueueWindowMessage(
					a_window,
					a_message,
					a_wParam,
					a_lParam,
					true));
			}

			if (ShouldConsumeModalMessage(a_message, a_wParam, a_lParam)) {
				return 0;
			}
			return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
		}
	}

	InitializeResult Initialize()
	{
		const auto currentThreadID = ::GetCurrentThreadId();
		if (Shared().SubclassActive.load(std::memory_order_acquire)) {
			const auto window = Shared().InitializedHostWindow.load(std::memory_order_relaxed);
			const auto threadID =
				Shared().InitializedHostWindowThreadID.load(std::memory_order_acquire);
			if (threadID != currentThreadID) {
				return InitializeResult::Deferred;
			}
			if (window && threadID == currentThreadID && ::IsWindow(window) &&
				VerifySubclass(window)) {
				return InitializeResult::Ready;
			}

			if (FirstFailure(subclassDriftLogged)) {
				logger::critical(
					"Win32 window subclass state no longer matches its verified HWND/thread");
			}
			InvalidateHostWindow();
			return InitializeResult::Failed;
		}

		const auto search = FindHostWindow();
		if (search.CandidateCount == 0) {
			static bool warned{};
			if (!warned) {
				warned = true;
				logger::warn("Win32 input deferred: no visible Starfield process window is ready");
			}
			return InitializeResult::Deferred;
		}
		if (search.CandidateCount != 1) {
			if (FirstFailure(ambiguousWindowLogged)) {
				logger::critical(
					"Win32 input rejected: expected one Starfield process window, found {}",
					search.CandidateCount);
			}
			return InitializeResult::Failed;
		}
		if (search.ThreadID != currentThreadID) {
			return InitializeResult::Deferred;
		}

		if (!::SetWindowSubclass(search.Window, &WindowSubclass, SubclassID(), 0)) {
			if (FirstFailure(subclassInstallFailureLogged)) {
				logger::critical("Failed to install the Starfield window subclass");
			}
			return InitializeResult::Failed;
		}

		if (!VerifySubclass(search.Window)) {
			const bool removed =
				::RemoveWindowSubclass(search.Window, &WindowSubclass, SubclassID()) != FALSE &&
				!VerifySubclass(search.Window);
			if (FirstFailure(subclassVerificationFailureLogged)) {
				logger::critical(
					"Starfield window-subclass verification failed; rollback {}",
					removed ? "succeeded" : "failed");
			}
			return InitializeResult::Failed;
		}

		Shared().InitializedHostWindow.store(search.Window, std::memory_order_relaxed);
		Shared().InitializedHostWindowThreadID.store(search.ThreadID, std::memory_order_release);
		Shared().HostWindowTearingDown.store(false, std::memory_order_release);
		Shared().HostCallbackPending.store(false, std::memory_order_release);
		ResetKeyboardToggleState();
		ResetRawMouseState();
		Shared().AcceptInput.store(false, std::memory_order_release);
		Shared().PointerRouteState.store(PointerRoute::Disabled, std::memory_order_release);
		Shared().EarlyRawMouseGeneration.store(0, std::memory_order_release);
		Shared().RawMouseFaultGeneration.store(0, std::memory_order_release);
		Shared().SubclassActive.store(true, std::memory_order_release);
		RequestInputReset();
		static_cast<void>(PostHostWindowCallback());
		return InitializeResult::Ready;
	}

	bool IsCurrentThreadHostWindowThread()
	{
		const auto initializedThreadID =
			Shared().InitializedHostWindowThreadID.load(std::memory_order_acquire);
		if (initializedThreadID != 0) {
			if (initializedThreadID != ::GetCurrentThreadId()) {
				return false;
			}
			if (Shared().HostWindowTearingDown.load(std::memory_order_acquire)) {
				return true;
			}
			const auto initializedWindow =
				Shared().InitializedHostWindow.load(std::memory_order_relaxed);
			DWORD processID{};
			const auto currentWindowThreadID = initializedWindow ?
				::GetWindowThreadProcessId(initializedWindow, &processID) :
				0;
			return currentWindowThreadID == initializedThreadID &&
			       processID == ::GetCurrentProcessId() &&
			       initializedThreadID == ::GetCurrentThreadId();
		}

		const auto search = FindHostWindow();
		return search.CandidateCount == 1 &&
		       search.ThreadID == ::GetCurrentThreadId();
	}

	bool IsInitialized() noexcept
	{
		return !Shared().HostWindowTearingDown.load(std::memory_order_acquire) &&
		       Shared().SubclassActive.load(std::memory_order_acquire) &&
		       Shared().InitializedHostWindow.load(std::memory_order_relaxed) != nullptr;
	}

	bool HasLiveBackend() noexcept
	{
		return Shared().BackendAlive.load(std::memory_order_acquire);
	}

	bool IsHostWindowUsable() noexcept
	{
		const auto window = Shared().InitializedHostWindow.load(std::memory_order_acquire);
		if (Shared().HostWindowTearingDown.load(std::memory_order_acquire) ||
			!Shared().SubclassActive.load(std::memory_order_acquire) || !window ||
			!::IsWindow(window) || !::IsWindowVisible(window) || ::IsIconic(window) ||
			::GetForegroundWindow() != window) {
			return false;
		}

		RECT clientArea{};
		return ReadClientArea(window, clientArea);
	}
	void SetHostWindowCallback(HostWindowCallback a_callback) noexcept
	{
		Shared().HostWindowCallbackFunction.store(a_callback, std::memory_order_release);
		if (!a_callback) {
			Shared().HostCallbackPending.store(false, std::memory_order_release);
		}
	}

	bool PostHostWindowCallback() noexcept
	{
		if (Shared().HostWindowTearingDown.load(std::memory_order_acquire) ||
			!Shared().HostWindowCallbackFunction.load(std::memory_order_acquire)) {
			return false;
		}

		const auto window = Shared().InitializedHostWindow.load(std::memory_order_acquire);
		const auto message = HostWindowCallbackMessage();
		if (!Shared().SubclassActive.load(std::memory_order_acquire) || !window || message == 0) {
			return false;
		}

		bool expected = false;
		if (!Shared().HostCallbackPending.compare_exchange_strong(
				expected,
				true,
				std::memory_order_acq_rel)) {
			return true;
		}

		if (::PostMessageW(window, message, 0, 0)) {
			return true;
		}

		Shared().HostCallbackPending.store(false, std::memory_order_release);
		if (FirstFailure(callbackPostFailureLogged)) {
			logger::error("Failed to post the coalesced Starfield HWND callback");
		}
		return false;
	}
}
