#include "platform/win32/Win32Platform.h"
#include "platform/win32/Win32PlatformInternal.h"

#include "runtime/WindowManager.h"

#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
	HWND a_window, UINT a_message, WPARAM a_wParam, LPARAM a_lParam);

namespace SFSEMenuFramework::Win32Platform
{
	using namespace Detail;

	namespace
	{
		constexpr std::size_t inputQueueCapacity = 256;

		struct InputQueueState final
		{
			std::mutex                                          Mutex;
			std::array<QueuedWindowMessage, inputQueueCapacity> Messages{};
			std::size_t                                         Count{ 0 };
			bool                                                ResetRequested{ true };
		};

		struct DrainedInput final
		{
			std::array<QueuedWindowMessage, inputQueueCapacity> Messages{};
			std::size_t                                         Count{ 0 };
			std::uint64_t                                       StateGeneration{ 0 };
			bool                                                ResetRequested{ false };
		};

		struct RenderPlatformState final
		{
			HWND Window{ nullptr };
			bool BackendAlive{ false };
			bool BackendInitializationFailed{ false };
		};

		template <class T>
		[[nodiscard]] T& State()
		{
			static auto* state = new T();
			return *state;
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
			Shared().InputStateGeneration.fetch_add(
				1, std::memory_order_release);
		}

		// Caller holds the queue lock, including across an entire raw mouse batch.
		[[nodiscard]] bool AppendQueuedMessage(
			InputQueueState& a_queue, const QueuedWindowMessage& a_message) noexcept
		{
			if (a_queue.Count != 0 &&
				TryCoalesce(a_queue.Messages[a_queue.Count - 1], a_message)) {
				return true;
			}
			if (a_queue.Count == a_queue.Messages.size()) {
				InvalidateQueuedInput(a_queue);
				return false;
			}
			a_queue.Messages[a_queue.Count++] = a_message;
			return true;
		}
	}

	void Detail::RequestInputReset()
	{
		auto& queue = State<InputQueueState>();
		std::scoped_lock lock{ queue.Mutex };
		InvalidateQueuedInput(queue);
	}

	bool Detail::EnqueueWindowMessage(
		HWND a_window,
		UINT a_message,
		WPARAM a_wParam,
		LPARAM a_lParam,
		bool a_requiresAcceptedInput)
	{
		auto& queue = State<InputQueueState>();
		std::scoped_lock lock{ queue.Mutex };
		if (a_requiresAcceptedInput &&
			!Shared().AcceptInput.load(std::memory_order_acquire)) {
			return false;
		}

		return AppendQueuedMessage(queue, {
			.Window = a_window,
			.Message = a_message,
			.WParam = a_wParam,
			.LParam = a_lParam
		});
	}

	bool Detail::EnqueueRawMouseBatch(
		const std::array<QueuedWindowMessage, rawMouseMessageCapacity>& a_messages,
		std::size_t a_count,
		std::uint64_t a_generation)
	{
		if (a_count == 0 || a_count > a_messages.size()) {
			return a_count == 0;
		}

		auto& queue = State<InputQueueState>();
		std::scoped_lock lock{ queue.Mutex };
		if (!Shared().AcceptInput.load(std::memory_order_acquire) ||
			Shared().PointerRouteState.load(std::memory_order_acquire) !=
				PointerRoute::EarlyRaw ||
			Shared().EarlyRawMouseGeneration.load(std::memory_order_acquire) !=
				a_generation) {
			return false;
		}

		for (std::size_t index = 0; index < a_count; ++index) {
			if (!AppendQueuedMessage(queue, a_messages[index])) {
				return false;
			}
		}
		return true;
	}

	namespace
	{
		[[nodiscard]] DrainedInput DrainQueuedInput()
		{
			DrainedInput result;
			auto& queue = State<InputQueueState>();
			std::scoped_lock lock{ queue.Mutex };
			result.Count = queue.Count;
			std::copy_n(queue.Messages.begin(), queue.Count, result.Messages.begin());
			result.ResetRequested = queue.ResetRequested;
			result.StateGeneration =
				Shared().InputStateGeneration.load(
					std::memory_order_acquire);

			queue.Count = 0;
			queue.ResetRequested = false;
			return result;
		}
		void SendMouseReleaseMessages(HWND a_window)
		{
			constexpr std::array releases{
				std::pair{ WM_LBUTTONUP, WPARAM{} },
				std::pair{ WM_RBUTTONUP, WPARAM{} },
				std::pair{ WM_MBUTTONUP, WPARAM{} },
				std::pair{ WM_XBUTTONUP, static_cast<WPARAM>(MAKEWPARAM(0, XBUTTON1)) },
				std::pair{ WM_XBUTTONUP, static_cast<WPARAM>(MAKEWPARAM(0, XBUTTON2)) }
			};
			for (const auto [message, parameter] : releases) {
				ImGui_ImplWin32_WndProcHandler(a_window, message, parameter, 0);
			}
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
			// Win32 shutdown clears every viewport's RendererUserData, even with
			// multi-viewports disabled. The application-owned DX12 main viewport
			// must survive this platform-only rebind, including in-flight buffers.
			auto* viewport = ImGui::GetMainViewport();
			auto* rendererData = std::exchange(viewport->RendererUserData, nullptr);
			ImGui_ImplWin32_Shutdown();
			viewport->RendererUserData = rendererData;
			a_state.BackendAlive = false;
			a_state.BackendInitializationFailed = false;
			a_state.Window = nullptr;
			Shared().BackendAlive.store(false, std::memory_order_release);
		}
	}

	bool UpdateInputState(
		bool a_acceptInput,
		std::uint64_t a_earlyRawMouseGeneration)
	{
		const auto window = Shared().InitializedHostWindow.load(std::memory_order_acquire);
		const bool onHostWindowThread =
			window && IsCurrentThreadHostWindowThread();

		const auto disableInput = [&]() {
			{
				auto& queue = State<InputQueueState>();
				std::scoped_lock lock{ queue.Mutex };
				const bool changed =
					Shared().AcceptInput.load(std::memory_order_relaxed) ||
					Shared().PointerRouteState.load(std::memory_order_relaxed) !=
						PointerRoute::Disabled ||
					Shared().EarlyRawMouseGeneration.load(std::memory_order_relaxed) != 0;
				Shared().AcceptInput.store(false, std::memory_order_release);
				Shared().PointerRouteState.store(
					PointerRoute::Disabled,
					std::memory_order_release);
				Shared().EarlyRawMouseGeneration.store(0, std::memory_order_release);
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
			a_acceptInput && Shared().SubclassActive.load(std::memory_order_acquire) &&
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
			Shared().PointerRouteState.load(std::memory_order_acquire);
		const auto previousGeneration =
			Shared().EarlyRawMouseGeneration.load(std::memory_order_acquire);
		const bool previouslyAccepted =
			Shared().AcceptInput.load(std::memory_order_acquire);
		if (previouslyAccepted && previousRoute == desiredRoute &&
			previousGeneration == a_earlyRawMouseGeneration) {
			if (desiredRoute == PointerRoute::Legacy) {
				return true;
			}
			if (IsEarlyRawMouseReady(
					window, a_earlyRawMouseGeneration) &&
				Shared().RawMouseFaultGeneration.load(std::memory_order_acquire) !=
					a_earlyRawMouseGeneration) {
				return true;
			}
		}

		if (desiredRoute == PointerRoute::EarlyRaw) {
			if (Shared().RawMouseFaultGeneration.load(std::memory_order_acquire) ==
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

			if (!Shared().SubclassActive.load(std::memory_order_acquire) ||
				::GetForegroundWindow() != window ||
				!HasCurrentInputLease() ||
				!WindowManager::IsBlockingWindowOpenGeneration(
					a_earlyRawMouseGeneration)) {
				disableInput();
				return false;
			}

			auto& queue = State<InputQueueState>();
			{
				std::scoped_lock lock{ queue.Mutex };
				Shared().AcceptInput.store(false, std::memory_order_release);
				Shared().PointerRouteState.store(
					PointerRoute::EarlyRaw,
					std::memory_order_release);
				Shared().EarlyRawMouseGeneration.store(
					a_earlyRawMouseGeneration,
					std::memory_order_release);
				InvalidateQueuedInput(queue);
				queue.Messages[queue.Count++] = seedMessage;
				Shared().AcceptInput.store(true, std::memory_order_release);
			}
			return true;
		}

		const bool handingOffEarlyRaw =
			previouslyAccepted && previousRoute == PointerRoute::EarlyRaw;
		if (handingOffEarlyRaw) {
			if (!PlaceLegacyCursorForRawHandoff(window)) {
				disableInput();
				return false;
			}
		}
		ResetWindowThreadMouseState(window);
		DeactivateRawMouseState();
		if (!Shared().SubclassActive.load(std::memory_order_acquire) ||
			::GetForegroundWindow() != window) {
			disableInput();
			return false;
		}

		{
			auto& queue = State<InputQueueState>();
			std::scoped_lock lock{ queue.Mutex };
			Shared().AcceptInput.store(false, std::memory_order_release);
			Shared().PointerRouteState.store(PointerRoute::Legacy, std::memory_order_release);
			Shared().EarlyRawMouseGeneration.store(0, std::memory_order_release);
			InvalidateQueuedInput(queue);
			Shared().AcceptInput.store(true, std::memory_order_release);
		}
		return true;
	}

	bool PrepareFrame()
	{
		auto& state = State<RenderPlatformState>();
		const auto window = Shared().InitializedHostWindow.load(std::memory_order_acquire);
		if (!IsHostWindowUsable()) {
			// Like SKSE-MF 928e01a Hooks.cpp, focus loss clears input rather than
			// destroying ImGui. Keep the backends and GPU resources for resume.
			static_cast<void>(UpdateInputState(false));
			if (state.BackendAlive) {
				ResetBackendInput(state.Window, false);
				ImGui::GetIO().MouseDrawCursor = false;
			}
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
			Shared().BackendAlive.store(true, std::memory_order_release);
			RequestInputReset();
		}

		if (Shared().AcceptInput.load(std::memory_order_acquire) &&
			::GetForegroundWindow() != window) {
			static_cast<void>(UpdateInputState(false));
			static_cast<void>(PostHostWindowCallback());
		}

		const auto input = DrainQueuedInput();
		bool acceptingInput = Shared().AcceptInput.load(std::memory_order_acquire);
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

		const auto observedGeneration = input.StateGeneration;
		auto currentGeneration =
			Shared().InputStateGeneration.load(std::memory_order_acquire);
		if (currentGeneration != observedGeneration) {
			acceptingInput = Shared().AcceptInput.load(std::memory_order_acquire);
			ResetBackendInput(window, acceptingInput);
		}

		ImGui_ImplWin32_NewFrame();

		currentGeneration = Shared().InputStateGeneration.load(std::memory_order_acquire);
		if (currentGeneration != observedGeneration) {
			acceptingInput = Shared().AcceptInput.load(std::memory_order_acquire);
			ResetBackendInput(window, acceptingInput);
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
}
