#include "Win32Platform.h"

#include "D3D12Renderer.h"
#include "FrameworkSettings.h"
#include "InputCapture.h"
#include "WindowManager.h"

#include <backends/imgui_impl_win32.h>
#include <imgui.h>
#include <REX/W32/DINPUT.h>

#include <CommCtrl.h>
#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND   a_window,
	UINT   a_message,
	WPARAM a_wParam,
	LPARAM a_lParam);

namespace SFSEMenuFramework::Win32Platform
{
	namespace
	{
		constexpr std::size_t inputQueueCapacity = 256;
		constexpr auto doublePressThreshold = std::chrono::milliseconds{ 300 };
		constexpr UINT keyboardHoldThresholdMilliseconds = 401;
		constexpr UINT_PTR keyboardHoldTimerTag =
			static_cast<UINT_PTR>(0x53464D4600000000ULL);
		constexpr UINT_PTR keyboardHoldTimerTagMask =
			static_cast<UINT_PTR>(0xFFFFFFFF00000000ULL);
		constexpr std::size_t rawMouseMessageCapacity = 13;

		enum class PointerRoute : std::uint8_t
		{
			Disabled,
			EarlyRaw,
			Legacy
		};

		struct WindowSearch final
		{
			HWND        Window{ nullptr };
			RECT        ClientArea{};
			DWORD       ThreadID{ 0 };
			std::size_t CandidateCount{ 0 };
		};

		struct QueuedWindowMessage final
		{
			HWND   Window{ nullptr };
			UINT   Message{ 0 };
			WPARAM WParam{ 0 };
			LPARAM LParam{ 0 };
		};

		struct InputQueueState final
		{
			std::mutex                                          Mutex;
			std::array<QueuedWindowMessage, inputQueueCapacity> Messages{};
			std::size_t                                         Count{ 0 };
			std::uint64_t                                       CoalescedSinceDrain{ 0 };
			std::uint64_t                                       DroppedSinceDrain{ 0 };
			std::uint64_t                                       OverflowResetsSinceDrain{ 0 };
			bool                                                ResetRequested{ true };
		};

		struct DrainedInput final
		{
			std::array<QueuedWindowMessage, inputQueueCapacity> Messages{};
			std::size_t                                         Count{ 0 };
			std::uint64_t                                       Coalesced{ 0 };
			std::uint64_t                                       Dropped{ 0 };
			std::uint64_t                                       OverflowResets{ 0 };
			std::uint64_t                                       StateGeneration{ 0 };
			bool                                                ResetRequested{ false };
		};

		struct RenderPlatformState final
		{
			HWND Window{ nullptr };
			bool BackendAlive{ false };
			bool BackendInitializationFailed{ false };
		};

		struct WindowThreadMouseState final
		{
			std::uint32_t ButtonsDown{ 0 };
			int           TrackedArea{ 0 };
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

		enum class RawMouseReadStatus : std::uint8_t
		{
			NotMouse,
			Ready,
			Failed
		};

		struct RawMouseReadResult final
		{
			RawMouseReadStatus Status{ RawMouseReadStatus::NotMouse };
			RAWMOUSE           Mouse{};
		};

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

		std::atomic<bool>               subclassActive{ false };
		std::atomic<bool>               acceptInput{ false };
		std::atomic<bool>               backendAlive{ false };
		std::atomic<bool>               hostWindowTearingDown{ false };
		std::atomic<bool>               hostCallbackPending{ false };
		std::atomic<HWND>               initializedHostWindow{ nullptr };
		std::atomic<DWORD>              initializedHostWindowThreadID{ 0 };
		std::atomic<HostWindowCallback> hostWindowCallback{ nullptr };
		std::atomic<std::uint64_t>      inputStateGeneration{ 1 };
		std::atomic<PointerRoute>       pointerRoute{ PointerRoute::Disabled };
		std::atomic<std::uint64_t>      earlyRawMouseGeneration{ 0 };
		std::atomic<std::uint64_t>      rawMouseFaultGeneration{ 0 };
		std::atomic_flag                mouseMessageLogged{};
		std::atomic_flag                keyboardMessageLogged{};
		std::atomic_flag                characterMessageLogged{};
		std::atomic_flag                rawInputMessageLogged{};
		std::atomic_flag                coalescingLogged{};
		std::atomic_flag                callbackPostFailureLogged{};
		std::atomic_flag                mouseCaptureFailureLogged{};
		std::atomic_flag                mouseTrackingFailureLogged{};
		std::atomic_flag                subclassDriftLogged{};
		std::atomic_flag                ambiguousWindowLogged{};
		std::atomic_flag                subclassInstallFailureLogged{};
		std::atomic_flag                subclassVerificationFailureLogged{};
		std::atomic_flag                rawKeyboardReadFailureLogged{};
		std::atomic_flag                rawMouseReadFailureLogged{};
		std::atomic_flag                rawMouseMovementLogged{};
		std::atomic_flag                rawMouseAbsoluteLogged{};
		std::atomic_flag                rawMouseHandoffFailureLogged{};
		std::atomic_flag                keyboardHoldTimerFailureLogged{};
		std::atomic<std::uint32_t>      keyboardHoldTimerGeneration{ 0 };

		[[nodiscard]] InputQueueState& GetInputQueue()
		{
			static auto* state = new InputQueueState();
			return *state;
		}

		[[nodiscard]] RenderPlatformState& GetRenderState()
		{
			static auto* state = new RenderPlatformState();
			return *state;
		}

		[[nodiscard]] WindowThreadMouseState& GetWindowThreadMouseState()
		{
			static auto* state = new WindowThreadMouseState();
			return *state;
		}

		[[nodiscard]] RawMouseState& GetRawMouseState()
		{
			static auto* state = new RawMouseState();
			return *state;
		}

		void ResetRawMouseState() noexcept
		{
			GetRawMouseState() = {};
		}

		void DeactivateRawMouseState() noexcept
		{
			auto& state = GetRawMouseState();
			state.Generation = 0;
			state.ButtonsDown = 0;
			state.Initialized = false;
		}

		[[nodiscard]] KeyboardToggleState& GetKeyboardToggleState()
		{
			static auto* state = new KeyboardToggleState();
			return *state;
		}

		LRESULT CALLBACK WindowSubclass(
			HWND      a_window,
			UINT      a_message,
			WPARAM    a_wParam,
			LPARAM    a_lParam,
			UINT_PTR  a_subclassID,
			DWORD_PTR a_referenceData);

		[[nodiscard]] UINT_PTR SubclassID() noexcept
		{
			return reinterpret_cast<UINT_PTR>(&WindowSubclass);
		}

		[[nodiscard]] bool IsKeyMessage(UINT a_message) noexcept;

		void CancelKeyboardHold() noexcept
		{
			auto& state = GetKeyboardToggleState();
			if (state.HoldTimerID != 0 && state.HoldWindow) {
				static_cast<void>(::KillTimer(state.HoldWindow, state.HoldTimerID));
			}
			state.HoldDIK = 0;
			state.HoldEngineEventID = -1;
			state.HoldWindow = nullptr;
			state.HoldTimerID = 0;
			InputCapture::CancelPendingHeldKeyboardSuppression();
		}

		void ResetKeyboardToggleState() noexcept
		{
			auto& state = GetKeyboardToggleState();
			CancelKeyboardHold();
			InputCapture::CancelPendingKeyboardSuppression();
			state.Down.fill(false);
			state.LastPress = {};
			state.LastPressDIK = 0;
			state.HasLastPress = false;
		}

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
				case VK_CLEAR:
					id = VK_NUMPAD5;
					break;
				case VK_PRIOR:
					id = VK_NUMPAD9;
					break;
				case VK_NEXT:
					id = VK_NUMPAD3;
					break;
				case VK_END:
					id = VK_NUMPAD1;
					break;
				case VK_HOME:
					id = VK_NUMPAD7;
					break;
				case VK_LEFT:
					id = VK_NUMPAD4;
					break;
				case VK_UP:
					id = VK_NUMPAD8;
					break;
				case VK_RIGHT:
					id = VK_NUMPAD6;
					break;
				case VK_DOWN:
					id = VK_NUMPAD2;
					break;
				case VK_INSERT:
					id = VK_NUMPAD0;
					break;
				case VK_DELETE:
					id = VK_DECIMAL;
					break;
				default:
					break;
				}
			}

			if (id == 0 || id >= 0x100) {
				return std::nullopt;
			}
			return static_cast<std::int32_t>(id);
		}

		[[nodiscard]] std::optional<KeyboardTransition> ReadRawKeyboard(
			LPARAM a_lParam) noexcept
		{
			RAWINPUTHEADER header{};
			UINT headerSize = sizeof(header);
			const auto headerBytes = ::GetRawInputData(
				reinterpret_cast<HRAWINPUT>(a_lParam),
				RID_HEADER,
				&header,
				&headerSize,
				sizeof(RAWINPUTHEADER));
			if (headerBytes == static_cast<UINT>(-1) ||
				headerBytes != sizeof(header)) {
				if (!rawKeyboardReadFailureLogged.test_and_set(
						std::memory_order_relaxed)) {
					logger::warn("Starfield RAWINPUT header could not be read");
				}
				return std::nullopt;
			}
			if (header.dwType != RIM_TYPEKEYBOARD) {
				return std::nullopt;
			}

			RAWINPUT input{};
			UINT size = sizeof(input);
			const auto copied = ::GetRawInputData(
				reinterpret_cast<HRAWINPUT>(a_lParam),
				RID_INPUT,
				&input,
				&size,
				sizeof(RAWINPUTHEADER));
			if (copied == static_cast<UINT>(-1) ||
				copied < sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD) ||
				input.header.dwType != RIM_TYPEKEYBOARD) {
				if (!rawKeyboardReadFailureLogged.test_and_set(
						std::memory_order_relaxed)) {
					logger::warn("Starfield raw keyboard packet could not be read");
				}
				return std::nullopt;
			}

			const auto& keyboard = input.data.keyboard;
			if (keyboard.MakeCode == 0 ||
				keyboard.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE ||
				keyboard.VKey >= 0xFF) {
				return std::nullopt;
			}

			const bool e0 = (keyboard.Flags & RI_KEY_E0) != 0;
			const bool e1 = (keyboard.Flags & RI_KEY_E1) != 0;
			if (e0 && e1) {
				return std::nullopt;
			}

			std::uint32_t dik{};
			if (e1) {
				if (keyboard.MakeCode != 0x45 || keyboard.VKey != VK_PAUSE) {
					return std::nullopt;
				}
				dik = REX::W32::DIK_PAUSE;
			} else {
				dik = keyboard.MakeCode & 0x7FU;
				if (e0) {
					dik |= 0x80U;
				}
			}

			if (dik == 0 || dik >= 0x100) {
				return std::nullopt;
			}
			const auto engineEventID = NormalizeStarfieldKeyboardEventID(
				keyboard.MakeCode,
				keyboard.Flags,
				keyboard.VKey);
			return KeyboardTransition{
				.DIK = dik,
				.EngineEventID = engineEventID.value_or(-1),
				.Down = (keyboard.Flags & RI_KEY_BREAK) == 0
			};
		}

		[[nodiscard]] RawMouseReadResult ReadRawMouse(
			LPARAM a_lParam) noexcept
		{
			RAWINPUTHEADER header{};
			UINT headerSize = sizeof(header);
			const auto headerBytes = ::GetRawInputData(
				reinterpret_cast<HRAWINPUT>(a_lParam),
				RID_HEADER,
				&header,
				&headerSize,
				sizeof(RAWINPUTHEADER));
			if (headerBytes == static_cast<UINT>(-1) ||
				headerBytes != sizeof(header)) {
				if (!rawMouseReadFailureLogged.test_and_set(
						std::memory_order_relaxed)) {
					logger::warn(
						"Starfield RAWINPUT mouse header could not be read");
				}
				return { .Status = RawMouseReadStatus::Failed };
			}
			if (header.dwType != RIM_TYPEMOUSE) {
				return { .Status = RawMouseReadStatus::NotMouse };
			}

			RAWINPUT input{};
			UINT size = sizeof(input);
			const auto copied = ::GetRawInputData(
				reinterpret_cast<HRAWINPUT>(a_lParam),
				RID_INPUT,
				&input,
				&size,
				sizeof(RAWINPUTHEADER));
			if (copied == static_cast<UINT>(-1) ||
				copied < sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) ||
				input.header.dwType != RIM_TYPEMOUSE) {
				if (!rawMouseReadFailureLogged.test_and_set(
						std::memory_order_relaxed)) {
					logger::warn(
						"Starfield raw mouse packet could not be read");
				}
				return { .Status = RawMouseReadStatus::Failed };
			}

			return {
				.Status = RawMouseReadStatus::Ready,
				.Mouse = input.data.mouse
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
			const bool down =
				a_message == WM_KEYDOWN || a_message == WM_SYSKEYDOWN;
			if (!down) {
				flags |= RI_KEY_BREAK;
			}

			std::uint32_t dik{};
			if (a_wParam == VK_PAUSE) {
				dik = REX::W32::DIK_PAUSE;
			} else if (a_wParam == VK_SNAPSHOT) {
				dik = REX::W32::DIK_SYSRQ;
			} else {
				dik = makeCode;
				if ((flags & RI_KEY_E0) != 0) {
					dik |= 0x80U;
				}
			}

			if (dik == 0 || dik >= 0x100) {
				return std::nullopt;
			}
			const auto engineEventID = NormalizeStarfieldKeyboardEventID(
				makeCode,
				flags,
				static_cast<USHORT>(a_wParam));
			return KeyboardTransition{
				.DIK = dik,
				.EngineEventID = engineEventID.value_or(-1),
				.Down = down
			};
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
			if (generation == 0) {
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
			auto& state = GetKeyboardToggleState();
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
				state.HoldDIK = 0;
				state.HoldEngineEventID = -1;
				state.HoldWindow = nullptr;
				if (!keyboardHoldTimerFailureLogged.test_and_set(
						std::memory_order_relaxed)) {
					logger::warn("Failed to arm the configured keyboard hold timer");
				}
				return false;
			}
			state.HoldTimerID = installedTimerID;
			return true;
		}

		[[nodiscard]] bool ProcessKeyboardHoldTimer(
			HWND a_window,
			UINT_PTR a_timerID) noexcept
		{
			if (!IsKeyboardHoldTimerID(a_timerID)) {
				return false;
			}

			auto& state = GetKeyboardToggleState();
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
				InputCapture::IsKeyboardEdgeOperational() &&
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

		[[nodiscard]] bool ProcessKeyboardTransition(
			HWND                      a_window,
			const KeyboardTransition& a_transition) noexcept
		{
			auto& state = GetKeyboardToggleState();
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
			if (wasDown || !InputCapture::IsKeyboardEdgeOperational()) {
				return false;
			}

			const auto* mainWindow = WindowManager::GetMainWindow();
			if (!mainWindow) {
				return false;
			}
			const bool mainWindowOpen =
				mainWindow->IsOpen.load(std::memory_order_acquire);
			if (dik == REX::W32::DIK_ESCAPE && mainWindowOpen) {
				CancelKeyboardHold();
				return ApplyMainWindowKeyboardEdge(
					false,
					dik,
					a_transition.EngineEventID);
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
				CancelKeyboardHold();
				return ApplyMainWindowKeyboardEdge(
					false,
					dik,
					a_transition.EngineEventID);
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
				!::GetClientRect(a_window, &a_clientArea)) {
				return false;
			}

			return a_clientArea.right > a_clientArea.left &&
			       a_clientArea.bottom > a_clientArea.top;
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
				search.ClientArea = clientArea;
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

		[[nodiscard]] bool HasCurrentInputLease() noexcept
		{
			const auto generation = WindowManager::GetBlockingWindowOpenGeneration();
			return D3D12Renderer::HasRecentBlockingWindowFrame(generation) &&
			       WindowManager::IsBlockingWindowOpenGeneration(generation);
		}

		[[nodiscard]] bool IsKeyMessage(UINT a_message) noexcept
		{
			return a_message == WM_KEYDOWN || a_message == WM_KEYUP ||
			       a_message == WM_SYSKEYDOWN || a_message == WM_SYSKEYUP;
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
			switch (a_message) {
			case WM_MOUSEMOVE:
			case WM_NCMOUSEMOVE:
			case WM_MOUSELEAVE:
			case WM_NCMOUSELEAVE:
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_LBUTTONDBLCLK:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_RBUTTONDBLCLK:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MBUTTONDBLCLK:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
			case WM_XBUTTONDBLCLK:
			case WM_MOUSEWHEEL:
			case WM_MOUSEHWHEEL:
				return true;
			default:
				return false;
			}
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

			switch (a_message) {
			case WM_MOUSELEAVE:
			case WM_NCMOUSELEAVE:
			case WM_INPUTLANGCHANGE:
				return false;
			default:
				return IsLegacyMouseMessage(a_message) || IsKeyMessage(a_message) ||
				       a_message == WM_CHAR;
			}
		}

		void LogInputMessageOnce(UINT a_message)
		{
			if (IsLegacyMouseMessage(a_message)) {
				if (!mouseMessageLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Win32 input observed: queued legacy mouse messages");
				}
				return;
			}

			if (IsKeyMessage(a_message)) {
				if (!keyboardMessageLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Win32 input observed: queued legacy keyboard messages");
				}
				return;
			}

			if (a_message == WM_CHAR) {
				if (!characterMessageLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Win32 input observed: queued character messages");
				}
				return;
			}

			if (a_message == WM_INPUT &&
				!rawInputMessageLogged.test_and_set(std::memory_order_relaxed)) {
				logger::info("Win32 input observed: raw-input messages remain chained to Starfield");
			}
		}

		[[nodiscard]] bool TryCoalesce(
			QueuedWindowMessage&       a_previous,
			const QueuedWindowMessage& a_next) noexcept
		{
			if (a_previous.Window != a_next.Window ||
				a_previous.Message != a_next.Message) {
				return false;
			}

			if (a_next.Message == WM_MOUSEMOVE || a_next.Message == WM_NCMOUSEMOVE) {
				a_previous = a_next;
				return true;
			}

			if ((a_next.Message == WM_MOUSEWHEEL ||
				 a_next.Message == WM_MOUSEHWHEEL) &&
				LOWORD(a_previous.WParam) == LOWORD(a_next.WParam)) {
				const auto previousDelta =
					static_cast<int>(static_cast<SHORT>(HIWORD(a_previous.WParam)));
				const auto nextDelta =
					static_cast<int>(static_cast<SHORT>(HIWORD(a_next.WParam)));
				const auto combinedDelta = previousDelta + nextDelta;
				if (combinedDelta >= (std::numeric_limits<SHORT>::min)() &&
					combinedDelta <= (std::numeric_limits<SHORT>::max)()) {
					a_previous.WParam = MAKEWPARAM(
						LOWORD(a_next.WParam),
						static_cast<WORD>(static_cast<SHORT>(combinedDelta)));
					a_previous.LParam = a_next.LParam;
					return true;
				}
			}

			return false;
		}

		void InvalidateQueuedInput(InputQueueState& a_queue) noexcept
		{
			a_queue.Count = 0;
			a_queue.ResetRequested = true;
			inputStateGeneration.fetch_add(1, std::memory_order_release);
		}

		void RequestInputReset()
		{
			auto& queue = GetInputQueue();
			std::scoped_lock lock{ queue.Mutex };
			InvalidateQueuedInput(queue);
		}

		[[nodiscard]] bool EnqueueWindowMessage(
			HWND a_window,
			UINT a_message,
			WPARAM a_wParam,
			LPARAM a_lParam,
			bool a_requiresAcceptedInput)
		{
			auto& queue = GetInputQueue();
			std::scoped_lock lock{ queue.Mutex };
			if (a_requiresAcceptedInput &&
				!acceptInput.load(std::memory_order_acquire)) {
				return false;
			}

			const QueuedWindowMessage message{
				.Window = a_window,
				.Message = a_message,
				.WParam = a_wParam,
				.LParam = a_lParam
			};
			if (queue.Count != 0 &&
				TryCoalesce(queue.Messages[queue.Count - 1], message)) {
				++queue.CoalescedSinceDrain;
				return true;
			}

			if (queue.Count == queue.Messages.size()) {
				queue.DroppedSinceDrain += queue.Count + 1;
				++queue.OverflowResetsSinceDrain;
				InvalidateQueuedInput(queue);
				return false;
			}

			queue.Messages[queue.Count++] = message;
			return true;
		}

		[[nodiscard]] bool EnqueueRawMouseBatch(
			const std::array<QueuedWindowMessage, rawMouseMessageCapacity>& a_messages,
			std::size_t a_count,
			std::uint64_t a_generation)
		{
			if (a_count == 0 || a_count > a_messages.size()) {
				return a_count == 0;
			}

			auto& queue = GetInputQueue();
			std::scoped_lock lock{ queue.Mutex };
			if (!acceptInput.load(std::memory_order_acquire) ||
				pointerRoute.load(std::memory_order_acquire) !=
					PointerRoute::EarlyRaw ||
				earlyRawMouseGeneration.load(std::memory_order_acquire) !=
					a_generation) {
				return false;
			}

			auto stagedMessages = queue.Messages;
			auto stagedCount = queue.Count;
			std::uint64_t coalesced{};
			for (std::size_t index = 0; index < a_count; ++index) {
				if (stagedCount != 0 &&
					TryCoalesce(stagedMessages[stagedCount - 1], a_messages[index])) {
					++coalesced;
					continue;
				}
				if (stagedCount == stagedMessages.size()) {
					queue.DroppedSinceDrain += queue.Count + a_count;
					++queue.OverflowResetsSinceDrain;
					InvalidateQueuedInput(queue);
					return false;
				}
				stagedMessages[stagedCount++] = a_messages[index];
			}
			queue.Messages = stagedMessages;
			queue.Count = stagedCount;
			queue.CoalescedSinceDrain += coalesced;
			return true;
		}

		[[nodiscard]] DrainedInput DrainQueuedInput()
		{
			DrainedInput result;
			auto& queue = GetInputQueue();
			std::scoped_lock lock{ queue.Mutex };
			result.Count = queue.Count;
			for (std::size_t index = 0; index < queue.Count; ++index) {
				result.Messages[index] = queue.Messages[index];
			}
			result.Coalesced = queue.CoalescedSinceDrain;
			result.Dropped = queue.DroppedSinceDrain;
			result.OverflowResets = queue.OverflowResetsSinceDrain;
			result.ResetRequested = queue.ResetRequested;
			result.StateGeneration =
				inputStateGeneration.load(std::memory_order_acquire);

			queue.Count = 0;
			queue.CoalescedSinceDrain = 0;
			queue.DroppedSinceDrain = 0;
			queue.OverflowResetsSinceDrain = 0;
			queue.ResetRequested = false;
			return result;
		}

		void ReportQueueTelemetry(const DrainedInput& a_input)
		{
			if (a_input.Coalesced != 0 &&
				!coalescingLogged.test_and_set(std::memory_order_relaxed)) {
				logger::info(
					"Win32 input queue coalescing is active (capacity {})",
					inputQueueCapacity);
			}
			if (a_input.OverflowResets != 0) {
				logger::warn(
					"Win32 input queue overflow: dropped {} messages and reset {} batch(es) "
					"(capacity {}, coalesced before drain {})",
					a_input.Dropped,
					a_input.OverflowResets,
					inputQueueCapacity,
					a_input.Coalesced);
			}
		}

		[[nodiscard]] std::uint32_t MouseButtonMask(
			UINT a_message,
			WPARAM a_wParam) noexcept
		{
			switch (a_message) {
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_LBUTTONDBLCLK:
				return 1U << 0;
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_RBUTTONDBLCLK:
				return 1U << 1;
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MBUTTONDBLCLK:
				return 1U << 2;
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
			case WM_XBUTTONDBLCLK:
				return HIWORD(a_wParam) == XBUTTON1 ? 1U << 3 : 1U << 4;
			default:
				return 0;
			}
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
			auto& state = GetWindowThreadMouseState();
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
			} else if (!mouseTrackingFailureLogged.test_and_set(std::memory_order_relaxed)) {
				logger::warn("Starfield HWND mouse tracking could not be armed");
			}
		}

		void ResetWindowThreadMouseState(HWND a_window)
		{
			auto& state = GetWindowThreadMouseState();
			CancelMouseTracking(a_window, state);
			state.ButtonsDown = 0;
			if (::GetCapture() == a_window) {
				static_cast<void>(::ReleaseCapture());
			}
		}

		void UpdateWindowThreadMouseState(
			HWND a_window,
			UINT a_message,
			WPARAM a_wParam)
		{
			auto& state = GetWindowThreadMouseState();
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
						!mouseCaptureFailureLogged.test_and_set(std::memory_order_relaxed)) {
						logger::warn("Starfield HWND mouse capture could not be acquired");
					}
				}
				state.ButtonsDown |= buttonMask;
				return;
			}

			if (IsMouseButtonUp(a_message)) {
				state.ButtonsDown &= ~buttonMask;
				if (state.ButtonsDown == 0 && ::GetCapture() == a_window) {
					static_cast<void>(::ReleaseCapture());
				}
			}
		}

		[[nodiscard]] WORD RawMouseKeyState(
			std::uint32_t a_buttonsDown) noexcept
		{
			WORD result{};
			if ((a_buttonsDown & (1U << 0)) != 0) {
				result |= MK_LBUTTON;
			}
			if ((a_buttonsDown & (1U << 1)) != 0) {
				result |= MK_RBUTTON;
			}
			if ((a_buttonsDown & (1U << 2)) != 0) {
				result |= MK_MBUTTON;
			}
			if ((a_buttonsDown & (1U << 3)) != 0) {
				result |= MK_XBUTTON1;
			}
			if ((a_buttonsDown & (1U << 4)) != 0) {
				result |= MK_XBUTTON2;
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

		void AppendRawMouseMessage(
			std::array<QueuedWindowMessage, rawMouseMessageCapacity>& a_messages,
			std::size_t& a_count,
			HWND a_window,
			UINT a_message,
			WPARAM a_wParam,
			LPARAM a_lParam) noexcept
		{
			if (a_count >= a_messages.size()) {
				return;
			}
			a_messages[a_count++] = {
				.Window = a_window,
				.Message = a_message,
				.WParam = a_wParam,
				.LParam = a_lParam
			};
		}

		[[nodiscard]] bool UpdateRawMousePosition(
			HWND a_window,
			const RAWMOUSE& a_mouse,
			RawMouseState& a_state,
			bool& a_positionChanged) noexcept
		{
			RECT clientArea{};
			if (!::GetClientRect(a_window, &clientArea) ||
				clientArea.right <= clientArea.left ||
				clientArea.bottom <= clientArea.top ||
				clientArea.left < 0 || clientArea.top < 0 ||
				clientArea.right - 1 > (std::numeric_limits<SHORT>::max)() ||
				clientArea.bottom - 1 > (std::numeric_limits<SHORT>::max)()) {
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
				if (!rawMouseAbsoluteLogged.test_and_set(
						std::memory_order_relaxed)) {
					logger::info(
						"Early raw mouse route mapped absolute device coordinates");
				}
			} else {
				a_state.X += a_mouse.lLastX;
				a_state.Y += a_mouse.lLastY;
			}

			a_state.X = std::clamp<std::int64_t>(
				a_state.X,
				clientArea.left,
				clientArea.right - 1);
			a_state.Y = std::clamp<std::int64_t>(
				a_state.Y,
				clientArea.top,
				clientArea.bottom - 1);
			a_positionChanged =
				a_state.X != previousX || a_state.Y != previousY;
			return true;
		}

		void AppendRawButtonTransition(
			std::array<QueuedWindowMessage, rawMouseMessageCapacity>& a_messages,
			std::size_t& a_count,
			HWND a_window,
			RawMouseState& a_state,
			USHORT a_rawFlags,
			USHORT a_downFlag,
			USHORT a_upFlag,
			std::uint32_t a_buttonMask,
			UINT a_downMessage,
			UINT a_upMessage,
			WORD a_xButton = 0) noexcept
		{
			const auto position = RawMousePositionParameter(a_state);
			if ((a_rawFlags & a_downFlag) != 0) {
				a_state.ButtonsDown |= a_buttonMask;
				const auto keys = RawMouseKeyState(a_state.ButtonsDown);
				AppendRawMouseMessage(
					a_messages,
					a_count,
					a_window,
					a_downMessage,
					a_xButton != 0 ? MAKEWPARAM(keys, a_xButton) : keys,
					position);
			}
			if ((a_rawFlags & a_upFlag) != 0) {
				a_state.ButtonsDown &= ~a_buttonMask;
				const auto keys = RawMouseKeyState(a_state.ButtonsDown);
				AppendRawMouseMessage(
					a_messages,
					a_count,
					a_window,
					a_upMessage,
					a_xButton != 0 ? MAKEWPARAM(keys, a_xButton) : keys,
					position);
			}
		}

		[[nodiscard]] bool ProcessEarlyRawMouse(
			HWND a_window,
			const RAWMOUSE& a_mouse) noexcept
		{
			auto& state = GetRawMouseState();
			const auto generation =
				earlyRawMouseGeneration.load(std::memory_order_acquire);
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

			std::array<QueuedWindowMessage, rawMouseMessageCapacity> messages{};
			std::size_t count{};
			const bool hasButtonsOrWheel = a_mouse.usButtonFlags != 0;
			if (positionChanged || hasButtonsOrWheel) {
				AppendRawMouseMessage(
					messages,
					count,
					a_window,
					WM_MOUSEMOVE,
					RawMouseKeyState(state.ButtonsDown),
					RawMousePositionParameter(state));
			}

			AppendRawButtonTransition(
				messages, count, a_window, state, a_mouse.usButtonFlags,
				RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 1U << 0,
				WM_LBUTTONDOWN, WM_LBUTTONUP);
			AppendRawButtonTransition(
				messages, count, a_window, state, a_mouse.usButtonFlags,
				RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 1U << 1,
				WM_RBUTTONDOWN, WM_RBUTTONUP);
			AppendRawButtonTransition(
				messages, count, a_window, state, a_mouse.usButtonFlags,
				RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 1U << 2,
				WM_MBUTTONDOWN, WM_MBUTTONUP);
			AppendRawButtonTransition(
				messages, count, a_window, state, a_mouse.usButtonFlags,
				RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 1U << 3,
				WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON1);
			AppendRawButtonTransition(
				messages, count, a_window, state, a_mouse.usButtonFlags,
				RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 1U << 4,
				WM_XBUTTONDOWN, WM_XBUTTONUP, XBUTTON2);

			const auto keys = RawMouseKeyState(state.ButtonsDown);
			if ((a_mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0) {
				AppendRawMouseMessage(
					messages,
					count,
					a_window,
					WM_MOUSEWHEEL,
					MAKEWPARAM(keys, a_mouse.usButtonData),
					RawMousePositionParameter(state));
			}
			if ((a_mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0) {
				AppendRawMouseMessage(
					messages,
					count,
					a_window,
					WM_MOUSEHWHEEL,
					MAKEWPARAM(keys, a_mouse.usButtonData),
					RawMousePositionParameter(state));
			}

			if (count == 0) {
				return true;
			}
			for (std::size_t index = 0; index < count; ++index) {
				UpdateWindowThreadMouseState(
					a_window,
					messages[index].Message,
					messages[index].WParam);
			}
			if (!EnqueueRawMouseBatch(messages, count, generation)) {
				state.ButtonsDown = 0;
				ResetWindowThreadMouseState(a_window);
				return false;
			}
			if (positionChanged &&
				!rawMouseMovementLogged.test_and_set(std::memory_order_relaxed)) {
				logger::info(
					"Early raw mouse route queued its first relative movement");
			}
			return true;
		}

		[[nodiscard]] bool InitializeEarlyRawMouse(
			HWND a_window,
			std::uint64_t a_generation,
			QueuedWindowMessage& a_seedMessage) noexcept
		{
			if (!a_window || a_generation == 0 ||
				!WindowManager::IsBlockingWindowOpenGeneration(a_generation)) {
				return false;
			}

			RECT clientArea{};
			if (!::GetClientRect(a_window, &clientArea) ||
				clientArea.right <= clientArea.left ||
				clientArea.bottom <= clientArea.top ||
				clientArea.left < 0 || clientArea.top < 0 ||
				clientArea.right - 1 > (std::numeric_limits<SHORT>::max)() ||
				clientArea.bottom - 1 > (std::numeric_limits<SHORT>::max)()) {
				return false;
			}

			ResetWindowThreadMouseState(a_window);
			auto& state = GetRawMouseState();
			std::int64_t initialX{};
			std::int64_t initialY{};
			if (state.Window == a_window) {
				initialX = std::clamp<std::int64_t>(
					state.X,
					clientArea.left,
					clientArea.right - 1);
				initialY = std::clamp<std::int64_t>(
					state.Y,
					clientArea.top,
					clientArea.bottom - 1);
			} else {
				POINT cursorPosition{};
				if (::GetCursorPos(&cursorPosition) &&
					::ScreenToClient(a_window, &cursorPosition)) {
					initialX = std::clamp<std::int64_t>(
						cursorPosition.x,
						clientArea.left,
						clientArea.right - 1);
					initialY = std::clamp<std::int64_t>(
						cursorPosition.y,
						clientArea.top,
						clientArea.bottom - 1);
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
			state = {
				.Window = a_window,
				.Generation = a_generation,
				.X = initialX,
				.Y = initialY,
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

		[[nodiscard]] bool PlaceLegacyCursorForRawHandoff(
			HWND a_window,
			POINT& a_clientPosition) noexcept
		{
			RECT clientArea{};
			if (!a_window || !::GetClientRect(a_window, &clientArea) ||
				clientArea.right <= clientArea.left ||
				clientArea.bottom <= clientArea.top) {
				return false;
			}

			const auto& state = GetRawMouseState();
			if (state.Initialized && state.Window == a_window) {
				a_clientPosition.x = static_cast<LONG>(std::clamp<std::int64_t>(
					state.X,
					clientArea.left,
					clientArea.right - 1));
				a_clientPosition.y = static_cast<LONG>(std::clamp<std::int64_t>(
					state.Y,
					clientArea.top,
					clientArea.bottom - 1));
			} else {
				a_clientPosition = {
					clientArea.left +
						(clientArea.right - clientArea.left) / 2,
					clientArea.top +
						(clientArea.bottom - clientArea.top) / 2
				};
			}

			auto screenPosition = a_clientPosition;
			if (::ClientToScreen(a_window, &screenPosition) &&
				::SetCursorPos(screenPosition.x, screenPosition.y)) {
				logger::info(
					"Early raw mouse position handed off to Starfield cursor ownership at {},{}",
					a_clientPosition.x,
					a_clientPosition.y);
				return true;
			}

			if (!rawMouseHandoffFailureLogged.test_and_set(
					std::memory_order_relaxed)) {
				logger::critical(
					"Early raw mouse position could not be handed off to the Starfield cursor");
			}
			return false;
		}

		void SendMouseReleaseMessages(HWND a_window)
		{
			ImGui_ImplWin32_WndProcHandler(a_window, WM_LBUTTONUP, 0, 0);
			ImGui_ImplWin32_WndProcHandler(a_window, WM_RBUTTONUP, 0, 0);
			ImGui_ImplWin32_WndProcHandler(a_window, WM_MBUTTONUP, 0, 0);
			ImGui_ImplWin32_WndProcHandler(
				a_window,
				WM_XBUTTONUP,
				MAKEWPARAM(0, XBUTTON1),
				0);
			ImGui_ImplWin32_WndProcHandler(
				a_window,
				WM_XBUTTONUP,
				MAKEWPARAM(0, XBUTTON2),
				0);
		}

		void ResetBackendInput(HWND a_window, bool a_acceptingInput)
		{
			auto& io = ImGui::GetIO();
			SendMouseReleaseMessages(a_window);
			ImGui_ImplWin32_WndProcHandler(a_window, WM_MOUSELEAVE, 0, 0);
			ImGui_ImplWin32_WndProcHandler(a_window, WM_NCMOUSELEAVE, 0, 0);
			io.ClearEventsQueue();
			io.ClearInputKeys();
			ImGui_ImplWin32_WndProcHandler(
				a_window,
				a_acceptingInput ? WM_SETFOCUS : WM_KILLFOCUS,
				0,
				0);
		}

		void ShutdownBackend(RenderPlatformState& a_state)
		{
			if (!a_state.BackendAlive) {
				return;
			}

			ResetBackendInput(a_state.Window, false);
			ImGui_ImplWin32_Shutdown();
			a_state.BackendAlive = false;
			a_state.BackendInitializationFailed = false;
			a_state.Window = nullptr;
			backendAlive.store(false, std::memory_order_release);
			logger::info("ImGui Win32 backend shut down on the render path");
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

		LRESULT CALLBACK WindowSubclass(
			HWND      a_window,
			UINT      a_message,
			WPARAM    a_wParam,
			LPARAM    a_lParam,
			UINT_PTR  a_subclassID,
			DWORD_PTR)
		{
			if (a_subclassID != SubclassID() ||
				!subclassActive.load(std::memory_order_acquire)) {
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			const auto callbackMessage = HostWindowCallbackMessage();
			if (callbackMessage != 0 && a_message == callbackMessage) {
				const bool wasPending =
					hostCallbackPending.exchange(false, std::memory_order_acq_rel);
				if (wasPending) {
					if (const auto callback =
							hostWindowCallback.load(std::memory_order_acquire)) {
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
				if (const auto transition = ReadRawKeyboard(a_lParam)) {
					static_cast<void>(
						ProcessKeyboardTransition(a_window, *transition));
				}
				const bool acceptingRawInput =
					acceptInput.load(std::memory_order_acquire);
				const bool currentInputLease =
					acceptingRawInput && HasCurrentInputLease();
				if (acceptingRawInput && !currentInputLease) {
					static_cast<void>(UpdateInputState(false));
					static_cast<void>(PostHostWindowCallback());
					LogInputMessageOnce(a_message);
					return ::DefSubclassProc(
						a_window,
						a_message,
						a_wParam,
						a_lParam);
				}
				const bool earlyRawInput =
					pointerRoute.load(std::memory_order_acquire) ==
						PointerRoute::EarlyRaw &&
					currentInputLease;
				if (earlyRawInput) {
					const auto generation =
						earlyRawMouseGeneration.load(std::memory_order_acquire);
					const auto mouse = ReadRawMouse(a_lParam);
					const bool failed =
						mouse.Status == RawMouseReadStatus::Failed ||
						(mouse.Status == RawMouseReadStatus::Ready &&
						 !ProcessEarlyRawMouse(a_window, mouse.Mouse));
					if (failed) {
						if (generation != 0) {
							rawMouseFaultGeneration.store(
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
				LogInputMessageOnce(a_message);
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			if (a_message == WM_NCDESTROY) {
				static_cast<void>(UpdateInputState(false));
				ResetKeyboardToggleState();
				ResetRawMouseState();
				hostWindowTearingDown.store(true, std::memory_order_release);
				hostCallbackPending.store(false, std::memory_order_release);
				if (const auto callback =
						hostWindowCallback.load(std::memory_order_acquire)) {
					callback();
				}
				hostCallbackPending.store(false, std::memory_order_release);
				subclassActive.store(false, std::memory_order_release);
				initializedHostWindow.store(nullptr, std::memory_order_relaxed);
				initializedHostWindowThreadID.store(0, std::memory_order_release);
				RequestInputReset();
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			if (a_message == WM_CAPTURECHANGED || a_message == WM_CANCELMODE) {
				ResetWindowThreadMouseState(a_window);
				GetRawMouseState().ButtonsDown = 0;
				RequestInputReset();
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
				if (const auto transition =
						ReadLegacyKeyboard(a_message, a_wParam, a_lParam)) {
					static_cast<void>(
						ProcessKeyboardTransition(a_window, *transition));
				}
			}

			const bool modalInput =
				acceptInput.load(std::memory_order_acquire) && HasCurrentInputLease();
			if (acceptInput.load(std::memory_order_relaxed) && !modalInput) {
				static_cast<void>(UpdateInputState(false));
				static_cast<void>(PostHostWindowCallback());
			}

			if (!modalInput || !IsBackendInputMessage(a_message) ||
				IsAltF4(a_message, a_wParam, a_lParam)) {
				return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
			}

			LogInputMessageOnce(a_message);
			const bool suppressDuplicateLegacyMouse =
				IsLegacyMouseMessage(a_message) &&
				pointerRoute.load(std::memory_order_acquire) ==
					PointerRoute::EarlyRaw;
			if (IsLegacyMouseMessage(a_message) &&
				!suppressDuplicateLegacyMouse) {
				UpdateWindowThreadMouseState(a_window, a_message, a_wParam);
			}
			if (!suppressDuplicateLegacyMouse) {
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
		if (subclassActive.load(std::memory_order_acquire)) {
			const auto window = initializedHostWindow.load(std::memory_order_relaxed);
			const auto threadID =
				initializedHostWindowThreadID.load(std::memory_order_acquire);
			if (threadID != currentThreadID) {
				return InitializeResult::Deferred;
			}
			if (window && threadID == currentThreadID && ::IsWindow(window) &&
				VerifySubclass(window)) {
				return InitializeResult::Ready;
			}

			if (!subclassDriftLogged.test_and_set(std::memory_order_relaxed)) {
				logger::critical(
					"Win32 window subclass state no longer matches its verified HWND/thread");
			}
			static_cast<void>(UpdateInputState(false));
			ResetKeyboardToggleState();
			ResetRawMouseState();
			hostWindowTearingDown.store(true, std::memory_order_release);
			hostCallbackPending.store(false, std::memory_order_release);
			if (const auto callback =
					hostWindowCallback.load(std::memory_order_acquire)) {
				callback();
			}
			hostCallbackPending.store(false, std::memory_order_release);
			subclassActive.store(false, std::memory_order_release);
			initializedHostWindow.store(nullptr, std::memory_order_relaxed);
			initializedHostWindowThreadID.store(0, std::memory_order_release);
			RequestInputReset();
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
			if (!ambiguousWindowLogged.test_and_set(std::memory_order_relaxed)) {
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
			if (!subclassInstallFailureLogged.test_and_set(std::memory_order_relaxed)) {
				logger::critical("Failed to install the Starfield window subclass");
			}
			return InitializeResult::Failed;
		}

		if (!VerifySubclass(search.Window)) {
			const bool removed =
				::RemoveWindowSubclass(search.Window, &WindowSubclass, SubclassID()) != FALSE &&
				!VerifySubclass(search.Window);
			if (!subclassVerificationFailureLogged.test_and_set(
					std::memory_order_relaxed)) {
				logger::critical(
					"Starfield window-subclass verification failed; rollback {}",
					removed ? "succeeded" : "failed");
			}
			return InitializeResult::Failed;
		}

		initializedHostWindow.store(search.Window, std::memory_order_relaxed);
		initializedHostWindowThreadID.store(search.ThreadID, std::memory_order_release);
		hostWindowTearingDown.store(false, std::memory_order_release);
		hostCallbackPending.store(false, std::memory_order_release);
		ResetKeyboardToggleState();
		ResetRawMouseState();
		acceptInput.store(false, std::memory_order_release);
		pointerRoute.store(PointerRoute::Disabled, std::memory_order_release);
		earlyRawMouseGeneration.store(0, std::memory_order_release);
		rawMouseFaultGeneration.store(0, std::memory_order_release);
		subclassActive.store(true, std::memory_order_release);
		RequestInputReset();
		logger::info(
			"Starfield window subclass initialized without ImGui access "
			"(PID {}, thread {}, client {}x{}, candidates {})",
			::GetCurrentProcessId(),
			search.ThreadID,
			search.ClientArea.right - search.ClientArea.left,
			search.ClientArea.bottom - search.ClientArea.top,
			search.CandidateCount);
		static_cast<void>(PostHostWindowCallback());
		return InitializeResult::Ready;
	}

	bool IsCurrentThreadHostWindowThread()
	{
		const auto initializedThreadID =
			initializedHostWindowThreadID.load(std::memory_order_acquire);
		if (initializedThreadID != 0) {
			if (initializedThreadID != ::GetCurrentThreadId()) {
				return false;
			}
			if (hostWindowTearingDown.load(std::memory_order_acquire)) {
				return true;
			}
			const auto initializedWindow =
				initializedHostWindow.load(std::memory_order_relaxed);
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
		return !hostWindowTearingDown.load(std::memory_order_acquire) &&
		       subclassActive.load(std::memory_order_acquire) &&
		       initializedHostWindow.load(std::memory_order_relaxed) != nullptr;
	}

	bool HasLiveBackend() noexcept
	{
		return backendAlive.load(std::memory_order_acquire);
	}

	bool IsHostWindowUsable() noexcept
	{
		const auto window = initializedHostWindow.load(std::memory_order_acquire);
		if (hostWindowTearingDown.load(std::memory_order_acquire) ||
			!subclassActive.load(std::memory_order_acquire) || !window ||
			!::IsWindow(window) || !::IsWindowVisible(window) || ::IsIconic(window) ||
			::GetForegroundWindow() != window) {
			return false;
		}

		RECT clientArea{};
		return ::GetClientRect(window, &clientArea) &&
		       clientArea.right > clientArea.left &&
		       clientArea.bottom > clientArea.top;
	}

	bool UpdateInputState(
		bool a_acceptInput,
		std::uint64_t a_earlyRawMouseGeneration)
	{
		const auto window = initializedHostWindow.load(std::memory_order_acquire);
		const bool onHostWindowThread =
			window && IsCurrentThreadHostWindowThread();

		const auto disableInput = [&]() {
			{
				auto& queue = GetInputQueue();
				std::scoped_lock lock{ queue.Mutex };
				const bool changed =
					acceptInput.load(std::memory_order_relaxed) ||
					pointerRoute.load(std::memory_order_relaxed) !=
						PointerRoute::Disabled ||
					earlyRawMouseGeneration.load(std::memory_order_relaxed) != 0;
				acceptInput.store(false, std::memory_order_release);
				pointerRoute.store(
					PointerRoute::Disabled,
					std::memory_order_release);
				earlyRawMouseGeneration.store(0, std::memory_order_release);
				if (changed) {
					InvalidateQueuedInput(queue);
				}
			}
			if (onHostWindowThread) {
				ResetWindowThreadMouseState(window);
				DeactivateRawMouseState();
			}
		};

		const bool shouldAcceptInput =
			a_acceptInput && subclassActive.load(std::memory_order_acquire) &&
			window && ::GetForegroundWindow() == window;
		if (!shouldAcceptInput) {
			disableInput();
			return true;
		}
		if (!onHostWindowThread) {
			disableInput();
			return false;
		}

		const auto desiredRoute = a_earlyRawMouseGeneration != 0 ?
			PointerRoute::EarlyRaw :
			PointerRoute::Legacy;
		const auto previousRoute =
			pointerRoute.load(std::memory_order_acquire);
		const auto previousGeneration =
			earlyRawMouseGeneration.load(std::memory_order_acquire);
		const bool previouslyAccepted =
			acceptInput.load(std::memory_order_acquire);
		if (previouslyAccepted && previousRoute == desiredRoute &&
			previousGeneration == a_earlyRawMouseGeneration) {
			if (desiredRoute == PointerRoute::Legacy) {
				return true;
			}
			const auto& state = GetRawMouseState();
			if (state.Initialized && state.Window == window &&
				state.Generation == a_earlyRawMouseGeneration &&
				rawMouseFaultGeneration.load(std::memory_order_acquire) !=
					a_earlyRawMouseGeneration) {
				return true;
			}
		}

		if (desiredRoute == PointerRoute::EarlyRaw) {
			if (rawMouseFaultGeneration.load(std::memory_order_acquire) ==
					a_earlyRawMouseGeneration ||
				!HasCurrentInputLease() ||
				!WindowManager::IsBlockingWindowOpenGeneration(
					a_earlyRawMouseGeneration)) {
				disableInput();
				return false;
			}

			QueuedWindowMessage seedMessage{};
			if (!InitializeEarlyRawMouse(
					window,
					a_earlyRawMouseGeneration,
					seedMessage)) {
				disableInput();
				return false;
			}
			UpdateWindowThreadMouseState(
				window,
				seedMessage.Message,
				seedMessage.WParam);

			if (!subclassActive.load(std::memory_order_acquire) ||
				::GetForegroundWindow() != window ||
				!HasCurrentInputLease() ||
				!WindowManager::IsBlockingWindowOpenGeneration(
					a_earlyRawMouseGeneration)) {
				disableInput();
				return false;
			}

			auto& queue = GetInputQueue();
			{
				std::scoped_lock lock{ queue.Mutex };
				acceptInput.store(false, std::memory_order_release);
				pointerRoute.store(
					PointerRoute::EarlyRaw,
					std::memory_order_release);
				earlyRawMouseGeneration.store(
					a_earlyRawMouseGeneration,
					std::memory_order_release);
				InvalidateQueuedInput(queue);
				queue.Messages[queue.Count++] = seedMessage;
				acceptInput.store(true, std::memory_order_release);
			}
			return true;
		}

		const bool handingOffEarlyRaw =
			previouslyAccepted && previousRoute == PointerRoute::EarlyRaw;
		if (handingOffEarlyRaw) {
			POINT clientPosition{};
			if (!PlaceLegacyCursorForRawHandoff(window, clientPosition)) {
				disableInput();
				return false;
			}
		}
		ResetWindowThreadMouseState(window);
		DeactivateRawMouseState();
		if (!subclassActive.load(std::memory_order_acquire) ||
			::GetForegroundWindow() != window) {
			disableInput();
			return false;
		}

		{
			auto& queue = GetInputQueue();
			std::scoped_lock lock{ queue.Mutex };
			acceptInput.store(false, std::memory_order_release);
			pointerRoute.store(PointerRoute::Legacy, std::memory_order_release);
			earlyRawMouseGeneration.store(0, std::memory_order_release);
			InvalidateQueuedInput(queue);
			acceptInput.store(true, std::memory_order_release);
		}
		if (handingOffEarlyRaw) {
			logger::info(
				"Early raw mouse route committed its Starfield legacy cursor handoff");
		}
		return true;
	}

	bool PrepareFrame()
	{
		auto& state = GetRenderState();
		const auto window = initializedHostWindow.load(std::memory_order_acquire);
		if (!IsInitialized() || !IsHostWindowUsable()) {
			ShutdownBackend(state);
			return false;
		}

		if (state.BackendAlive && state.Window != window) {
			ShutdownBackend(state);
		}
		if (state.Window != window) {
			state.Window = window;
			state.BackendInitializationFailed = false;
		}
		if (!state.BackendAlive) {
			if (state.BackendInitializationFailed) {
				return false;
			}
			if (!ImGui_ImplWin32_Init(window)) {
				state.BackendInitializationFailed = true;
				logger::critical(
					"Failed to initialize the official Dear ImGui Win32 backend on render thread {}",
					::GetCurrentThreadId());
				return false;
			}

			state.BackendAlive = true;
			backendAlive.store(true, std::memory_order_release);
			RequestInputReset();
			logger::info(
				"ImGui Win32 backend initialized on render thread {} for HWND 0x{:X}",
				::GetCurrentThreadId(),
				reinterpret_cast<std::uintptr_t>(window));
		}

		if (acceptInput.load(std::memory_order_acquire) &&
			::GetForegroundWindow() != window) {
			static_cast<void>(UpdateInputState(false));
			static_cast<void>(PostHostWindowCallback());
		}

		const auto input = DrainQueuedInput();
		ReportQueueTelemetry(input);
		bool acceptingInput = acceptInput.load(std::memory_order_acquire);
		if (input.ResetRequested) {
			ResetBackendInput(window, acceptingInput);
		}

		for (std::size_t index = 0; index < input.Count; ++index) {
			const auto& message = input.Messages[index];
			if (message.Window == window) {
				ImGui_ImplWin32_WndProcHandler(
					message.Window,
					message.Message,
					message.WParam,
					message.LParam);
			}
		}

		auto observedGeneration = input.StateGeneration;
		auto currentGeneration =
			inputStateGeneration.load(std::memory_order_acquire);
		if (currentGeneration != observedGeneration) {
			acceptingInput = acceptInput.load(std::memory_order_acquire);
			ResetBackendInput(window, acceptingInput);
			observedGeneration = currentGeneration;
		}

		ImGui_ImplWin32_NewFrame();

		currentGeneration = inputStateGeneration.load(std::memory_order_acquire);
		if (currentGeneration != observedGeneration) {
			acceptingInput = acceptInput.load(std::memory_order_acquire);
			ResetBackendInput(window, acceptingInput);
			observedGeneration = currentGeneration;
		}

		if (acceptingInput && ::GetForegroundWindow() != window) {
			static_cast<void>(UpdateInputState(false));
			static_cast<void>(PostHostWindowCallback());
			acceptingInput = false;
			ResetBackendInput(window, false);
		}

		auto& io = ImGui::GetIO();
		io.MouseDrawCursor = acceptingInput;
		if (!acceptingInput) {
			io.ClearEventsQueue();
			io.ClearInputKeys();
		}
		if (!std::isfinite(io.DeltaTime) || io.DeltaTime <= 0.0F) {
			io.DeltaTime = 1.0F / 60.0F;
		} else {
			io.DeltaTime = std::clamp(io.DeltaTime, 0.001F, 0.1F);
		}
		return std::isfinite(io.DisplaySize.x) &&
		       std::isfinite(io.DisplaySize.y) &&
		       io.DisplaySize.x > 0.0F &&
		       io.DisplaySize.y > 0.0F;
	}

	void SetHostWindowCallback(HostWindowCallback a_callback) noexcept
	{
		hostWindowCallback.store(a_callback, std::memory_order_release);
		if (!a_callback) {
			hostCallbackPending.store(false, std::memory_order_release);
		}
	}

	bool PostHostWindowCallback() noexcept
	{
		if (hostWindowTearingDown.load(std::memory_order_acquire) ||
			!hostWindowCallback.load(std::memory_order_acquire)) {
			return false;
		}

		const auto window = initializedHostWindow.load(std::memory_order_acquire);
		const auto message = HostWindowCallbackMessage();
		if (!subclassActive.load(std::memory_order_acquire) || !window || message == 0) {
			return false;
		}

		bool expected = false;
		if (!hostCallbackPending.compare_exchange_strong(
				expected,
				true,
				std::memory_order_acq_rel)) {
			return true;
		}

		if (::PostMessageW(window, message, 0, 0)) {
			return true;
		}

		hostCallbackPending.store(false, std::memory_order_release);
		if (!callbackPostFailureLogged.test_and_set(std::memory_order_relaxed)) {
			logger::error("Failed to post the coalesced Starfield HWND callback");
		}
		return false;
	}
}
