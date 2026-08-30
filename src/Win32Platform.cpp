#include "Win32Platform.h"

#include "D3D12Renderer.h"
#include "WindowManager.h"

#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <CommCtrl.h>
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND   a_window,
	UINT   a_message,
	WPARAM a_wParam,
	LPARAM a_lParam);

namespace SFSEMenuFramework::Win32Platform
{
	namespace
	{
		struct WindowSearch final
		{
			HWND        Window{ nullptr };
			RECT        ClientArea{};
			DWORD       ThreadID{ 0 };
			std::size_t CandidateCount{ 0 };
		};

		struct PlatformState final
		{
			HWND Window{ nullptr };
			bool Initialized{ false };
			bool BackendAlive{ false };
			bool HasInputState{ false };
			bool WasAcceptingInput{ false };
		};

		std::atomic<bool> subclassActive{ false };
		std::atomic<bool> acceptInput{ false };
		std::atomic<HWND> initializedHostWindow{ nullptr };
		std::atomic<DWORD> initializedHostWindowThreadID{ 0 };
		std::atomic_flag  mouseMessageLogged{};
		std::atomic_flag  keyboardMessageLogged{};
		std::atomic_flag  characterMessageLogged{};
		std::atomic_flag  rawInputMessageLogged{};

		[[nodiscard]] PlatformState& GetState()
		{
			static auto* state = new PlatformState();
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
			const auto generation = WindowManager::GetMainWindowOpenGeneration();
			return D3D12Renderer::HasRecentMainWindowFrame(generation) &&
			       WindowManager::IsMainWindowOpenGeneration(generation);
		}

		void ToggleMainWindow()
		{
			auto* mainWindow = WindowManager::GetMainWindow();
			if (!mainWindow) {
				return;
			}

			const bool isOpen = WindowManager::ToggleMainWindow();
			logger::info("Mod Control Panel {}", isOpen ? "opened" : "closed");
		}

		[[nodiscard]] bool IsF1Message(UINT a_message, WPARAM a_wParam) noexcept
		{
			if (a_wParam != VK_F1) {
				return false;
			}
			return a_message == WM_KEYDOWN || a_message == WM_KEYUP ||
			       a_message == WM_SYSKEYDOWN || a_message == WM_SYSKEYUP;
		}

		[[nodiscard]] bool IsInitialKeyDown(UINT a_message, LPARAM a_lParam) noexcept
		{
			const bool isDown = a_message == WM_KEYDOWN || a_message == WM_SYSKEYDOWN;
			const auto bits = static_cast<std::uintptr_t>(a_lParam);
			return isDown && (bits & (std::uintptr_t{ 1 } << 30)) == 0;
		}

		void LogInputMessageOnce(UINT a_message)
		{
			switch (a_message) {
			case WM_MOUSEMOVE:
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_XBUTTONDOWN:
			case WM_XBUTTONUP:
			case WM_MOUSEWHEEL:
			case WM_MOUSEHWHEEL:
				if (!mouseMessageLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Win32 input observed: legacy mouse messages");
				}
				break;
			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_SYSKEYDOWN:
			case WM_SYSKEYUP:
				if (!keyboardMessageLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Win32 input observed: legacy keyboard messages");
				}
				break;
			case WM_CHAR:
				if (!characterMessageLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Win32 input observed: character messages");
				}
				break;
			case WM_INPUT:
				if (!rawInputMessageLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Win32 input observed: raw-input messages remain chained to Starfield");
				}
				break;
			default:
				break;
			}
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

			if (IsF1Message(a_message, a_wParam)) {
				if (IsInitialKeyDown(a_message, a_lParam)) {
					ToggleMainWindow();
				}
				return 0;
			}

			if (acceptInput.load(std::memory_order_acquire) &&
				HasCurrentInputLease()) {
				LogInputMessageOnce(a_message);
				D3D12Renderer::ProcessWindowMessage(
					a_window,
					a_message,
					a_wParam,
					a_lParam);
			}

			return ::DefSubclassProc(a_window, a_message, a_wParam, a_lParam);
		}
	}

	InitializeResult Initialize()
	{
		auto& state = GetState();
		if (state.Initialized) {
			return InitializeResult::Ready;
		}
		if (state.BackendAlive) {
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
			logger::critical(
				"Win32 input rejected: expected one Starfield process window, found {}",
				search.CandidateCount);
			return InitializeResult::Failed;
		}

		const auto currentThreadID = ::GetCurrentThreadId();
		if (search.ThreadID != currentThreadID) {
			logger::critical(
				"Win32 input rejected: window thread {} differs from SFSE task thread {}",
				search.ThreadID,
				currentThreadID);
			return InitializeResult::Failed;
		}

		if (!ImGui_ImplWin32_Init(search.Window)) {
			logger::critical("Failed to initialize the official Dear ImGui Win32 backend");
			return InitializeResult::Failed;
		}
		state.BackendAlive = true;

		if (!::SetWindowSubclass(search.Window, &WindowSubclass, SubclassID(), 0)) {
			logger::critical("Failed to install the Starfield window subclass");
			ImGui_ImplWin32_Shutdown();
			state.BackendAlive = false;
			return InitializeResult::Failed;
		}

		if (!VerifySubclass(search.Window)) {
			subclassActive.store(false, std::memory_order_release);
			const bool removed =
				::RemoveWindowSubclass(search.Window, &WindowSubclass, SubclassID()) != FALSE &&
				!VerifySubclass(search.Window);
			logger::critical(
				"Starfield window-subclass verification failed; rollback {}",
				removed ? "succeeded" : "failed, so the ImGui context must remain alive");
			if (removed) {
				ImGui_ImplWin32_Shutdown();
				state.BackendAlive = false;
			}
			return InitializeResult::Failed;
		}

		state.Window = search.Window;
		state.Initialized = true;
		initializedHostWindow.store(search.Window, std::memory_order_relaxed);
		initializedHostWindowThreadID.store(search.ThreadID, std::memory_order_release);
		subclassActive.store(true, std::memory_order_release);
		logger::info(
			"ImGui Win32 input initialized (PID {}, thread {}, client {}x{}, candidates {})",
			::GetCurrentProcessId(),
			search.ThreadID,
			search.ClientArea.right - search.ClientArea.left,
			search.ClientArea.bottom - search.ClientArea.top,
			search.CandidateCount);
		return InitializeResult::Ready;
	}

	bool IsCurrentThreadHostWindowThread()
	{
		const auto initializedThreadID =
			initializedHostWindowThreadID.load(std::memory_order_acquire);
		if (initializedThreadID != 0) {
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
		return GetState().Initialized;
	}

	bool HasLiveBackend() noexcept
	{
		return GetState().BackendAlive;
	}

	bool IsHostWindowUsable() noexcept
	{
		const auto window = GetState().Window;
		if (!window || !::IsWindow(window) || !::IsWindowVisible(window) || ::IsIconic(window)) {
			return false;
		}

		RECT clientArea{};
		return ::GetClientRect(window, &clientArea) &&
		       clientArea.right > clientArea.left &&
		       clientArea.bottom > clientArea.top;
	}

	void UpdateInputState(bool a_acceptInput)
	{
		auto& state = GetState();
		if (!state.Initialized) {
			return;
		}

		const bool shouldAcceptInput =
			a_acceptInput && ::GetForegroundWindow() == state.Window;

		if (state.HasInputState &&
			state.WasAcceptingInput == shouldAcceptInput) {
			return;
		}

		if (shouldAcceptInput) {
			auto& io = ImGui::GetIO();
			io.ClearEventsQueue();
			io.ClearInputKeys();
		}

		if (shouldAcceptInput) {
			ImGui_ImplWin32_WndProcHandler(state.Window, WM_SETFOCUS, 0, 0);
		} else {
			acceptInput.store(false, std::memory_order_release);
			SendMouseReleaseMessages(state.Window);
			ImGui_ImplWin32_WndProcHandler(state.Window, WM_MOUSELEAVE, 0, 0);
			ImGui_ImplWin32_WndProcHandler(state.Window, WM_NCMOUSELEAVE, 0, 0);
			ImGui_ImplWin32_WndProcHandler(state.Window, WM_KILLFOCUS, 0, 0);
			auto& io = ImGui::GetIO();
			io.ClearEventsQueue();
			io.ClearInputKeys();
		}

		acceptInput.store(shouldAcceptInput, std::memory_order_release);
		state.WasAcceptingInput = shouldAcceptInput;
		state.HasInputState = true;
	}

	bool PrepareFrame()
	{
		if (!IsInitialized() || !IsHostWindowUsable()) {
			return false;
		}

		ImGui_ImplWin32_NewFrame();
		auto& io = ImGui::GetIO();
		if (!acceptInput.load(std::memory_order_acquire)) {
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

	void ProcessWindowMessage(
		HWND   a_window,
		UINT   a_message,
		WPARAM a_wParam,
		LPARAM a_lParam)
	{
		if (!IsInitialized() ||
			!subclassActive.load(std::memory_order_acquire) ||
			!acceptInput.load(std::memory_order_acquire)) {
			return;
		}
		ImGui_ImplWin32_WndProcHandler(
			a_window,
			a_message,
			a_wParam,
			a_lParam);
	}
}
