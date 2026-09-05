#include "input/GamepadNavigation.h"

#include <RE/B/BSInputEventUser.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>

// The button mapping follows SKSE Menu Framework 3 src/Input.cpp at commit
// c97cdce6dd207c7cf1401611bc82bd7e8f97a814 (GPL-3.0). Starfield's normalized
// native thumbstick events replace Skyrim's BSInputDevice polling, and a
// bounded queue transfers them to the render thread. Directional analog
// submission follows Dear ImGui 1.90.8-docking imgui_impl_win32.cpp at commit
// 6d948ab47ecf984239af01434f3ed03808dbf188 (MIT).
namespace SFSEMenuFramework::GamepadNavigation
{
	namespace
	{
		constexpr std::size_t queueCapacity = 512;
		constexpr float       activityThreshold = 0.10F;
		constexpr float       gamepadRepeatDelay = 0.625F;
		constexpr float       gamepadRepeatRate = 0.125F;

		enum class EventKind : std::uint8_t
		{
			Button,
			LeftStick,
			RightStick
		};

		struct PendingEvent final
		{
			EventKind     Kind{};
			std::uint32_t ID{};
			float         X{};
			float         Y{};
			bool          InitialPress{};
		};

		struct CloseRequest final
		{
			ImGuiID       WindowID{};
			std::uint64_t Generation{};
		};

		struct Queue final
		{
			std::atomic_flag                       Lock{};
			std::array<PendingEvent, queueCapacity> Events{};
			std::size_t                            Count{};
			std::uint64_t                          Generation{};
			bool                                   Overflowed{};
		};

		Queue                       queue;
		std::atomic<std::uint64_t> lastInputStamp{};
		std::atomic<std::uint64_t> nativeGamepadGeneration{};
		std::atomic_flag           overflowLogged{};
		CloseRequest               pendingCloseRequest;

		class QueueLock final
		{
		public:
			explicit QueueLock(std::atomic_flag& a_lock) noexcept : lock(a_lock)
			{
				while (lock.test_and_set(std::memory_order_acquire)) {
					YieldProcessor();
				}
			}

			~QueueLock() { lock.clear(std::memory_order_release); }

			QueueLock(const QueueLock&) = delete;
			QueueLock& operator=(const QueueLock&) = delete;

		private:
			std::atomic_flag& lock;
		};

		[[nodiscard]] std::uint64_t MakeInputStamp(
			std::uint64_t a_generation, bool a_gamepad) noexcept
		{
			return (a_generation << 1) | static_cast<std::uint64_t>(a_gamepad);
		}

		[[nodiscard]] bool IsMappedButton(std::uint32_t a_id) noexcept
		{
			switch (a_id) {
			case 1:
			case 2:
			case 4:
			case 8:
			case 9:
			case 10:
			case 16:
			case 32:
			case 64:
			case 128:
			case 256:
			case 512:
			case 4096:
			case 8192:
			case 16384:
			case 32768:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] ImGuiKey MapButton(std::uint32_t a_id) noexcept
		{
			switch (a_id) {
			case 1: return ImGuiKey_GamepadDpadUp;
			case 2: return ImGuiKey_GamepadDpadDown;
			case 4: return ImGuiKey_GamepadDpadLeft;
			case 8: return ImGuiKey_GamepadDpadRight;
			case 9: return ImGuiKey_GamepadL2;
			case 10: return ImGuiKey_GamepadR2;
			case 16: return ImGuiKey_GamepadStart;
			case 32: return ImGuiKey_GamepadBack;
			case 64: return ImGuiKey_GamepadL3;
			case 128: return ImGuiKey_GamepadR3;
			case 256: return ImGuiKey_GamepadL1;
			case 512: return ImGuiKey_GamepadR1;
			case 4096: return ImGuiKey_GamepadFaceDown;
			case 8192: return ImGuiKey_GamepadFaceRight;
			case 16384: return ImGuiKey_GamepadFaceLeft;
			case 32768: return ImGuiKey_GamepadFaceUp;
			default: return ImGuiKey_None;
			}
		}

		void Enqueue(
			const PendingEvent& a_event, std::uint64_t a_generation) noexcept
		{
			QueueLock lock{ queue.Lock };
			if (queue.Generation != a_generation) {
				queue.Generation = a_generation;
				queue.Count = 0;
				queue.Overflowed = false;
			}
			if (queue.Overflowed) {
				return;
			}
			if (queue.Count == queue.Events.size()) {
				queue.Count = 0;
				queue.Overflowed = true;
				return;
			}
			queue.Events[queue.Count++] = a_event;
		}

		void AddDirection(
			ImGuiIO& a_io, ImGuiKey a_key, float a_value) noexcept
		{
			const float value = std::clamp(a_value, 0.0F, 1.0F);
			a_io.AddKeyAnalogEvent(a_key, value > activityThreshold, value);
		}

		void ApplyEvent(ImGuiIO& a_io, const PendingEvent& a_event) noexcept
		{
			if (a_event.Kind == EventKind::Button) {
				const auto key = MapButton(a_event.ID);
				if (key == ImGuiKey_GamepadL2 || key == ImGuiKey_GamepadR2) {
					AddDirection(a_io, key, a_event.X);
				} else if (key != ImGuiKey_None) {
					a_io.AddKeyEvent(key, a_event.X > 0.0F);
				}
				return;
			}

			const bool left = a_event.Kind == EventKind::LeftStick;
			AddDirection(
				a_io,
				left ? ImGuiKey_GamepadLStickLeft : ImGuiKey_GamepadRStickLeft,
				-a_event.X);
			AddDirection(
				a_io,
				left ? ImGuiKey_GamepadLStickRight : ImGuiKey_GamepadRStickRight,
				a_event.X);
			AddDirection(
				a_io,
				left ? ImGuiKey_GamepadLStickUp : ImGuiKey_GamepadRStickUp,
				a_event.Y);
			AddDirection(
				a_io,
				left ? ImGuiKey_GamepadLStickDown : ImGuiKey_GamepadRStickDown,
				-a_event.Y);
		}

		void RequestCloseIfCancelHasNoTarget(
			const PendingEvent& a_event, std::uint64_t a_generation) noexcept
		{
			// Preserve Dear ImGui's NavUpdateCancelRequest priority. Closing is
			// an SFSE fallback only after the current context has nothing to cancel.
			if (a_event.Kind != EventKind::Button || a_event.ID != 8192 ||
				!a_event.InitialPress) {
				return;
			}

			auto* context = ImGui::GetCurrentContext();
			if (!context) {
				return;
			}
			auto& state = *context;
			if (!state.NavWindow || state.ActiveId != 0 || state.NavId != 0 ||
				state.NavLayer != ImGuiNavLayer_Main ||
				state.NavWindowingTarget != nullptr ||
				state.OpenPopupStack.Size != 0) {
				return;
			}

			auto* root = state.NavWindow->RootWindowForNav;
			if (!root || (state.NavWindow != state.NavWindow->RootWindow &&
				!(root->Flags & ImGuiWindowFlags_Popup) && root->ParentWindow)) {
				return;
			}
			pendingCloseRequest = { root->ID, a_generation };
		}
	}

	void ObserveGamepadActivity(std::uint64_t a_generation) noexcept
	{
		if (a_generation == 0) {
			return;
		}
		nativeGamepadGeneration.store(a_generation, std::memory_order_release);
		lastInputStamp.store(
			MakeInputStamp(a_generation, true),
			std::memory_order_release);
	}

	void ObserveMouseActivity(std::uint64_t a_generation) noexcept
	{
		if (a_generation != 0) {
			lastInputStamp.store(
				MakeInputStamp(a_generation, false),
				std::memory_order_release);
		}
	}

	bool ShouldDrawMouseCursor(std::uint64_t a_generation) noexcept
	{
		if (a_generation == 0) {
			return true;
		}
		const auto stamp = lastInputStamp.load(std::memory_order_acquire);
		return (stamp >> 1) != a_generation || (stamp & 1) == 0;
	}

	void CaptureNativeEvent(
		const RE::InputEvent& a_event,
		std::uint64_t         a_generation,
		bool                  a_sendToImGui) noexcept
	{
		if (a_generation == 0 ||
			a_event.deviceType != RE::InputEvent::DeviceType::kGamepad) {
			return;
		}

		PendingEvent pending;
		bool         active{};
		if (a_event.eventType == RE::InputEvent::EventType::kButton) {
			const auto& button = static_cast<const RE::ButtonEvent&>(a_event);
			if (button.idCode < 0) {
				return;
			}
			pending.Kind = EventKind::Button;
			pending.ID = static_cast<std::uint32_t>(button.idCode);
			pending.X = std::clamp(button.value, 0.0F, 1.0F);
			pending.InitialPress =
				button.value != 0.0F && button.heldDownSecs == 0.0F;
			active = pending.X > activityThreshold;
			if (!IsMappedButton(pending.ID)) {
				if (active) {
					ObserveGamepadActivity(a_generation);
				}
				return;
			}
		} else if (a_event.eventType == RE::InputEvent::EventType::kThumbstick) {
			const auto& stick = static_cast<const RE::ThumbstickEvent&>(a_event);
			if (!stick.IsLeft() && !stick.IsRight()) {
				return;
			}
			pending.Kind = stick.IsLeft() ? EventKind::LeftStick : EventKind::RightStick;
			pending.X = std::clamp(stick.xValue, -1.0F, 1.0F);
			pending.Y = std::clamp(stick.yValue, -1.0F, 1.0F);
			active = std::abs(pending.X) > activityThreshold ||
				std::abs(pending.Y) > activityThreshold;
		} else {
			return;
		}

		if (active) {
			ObserveGamepadActivity(a_generation);
		}
		if (a_sendToImGui) {
			Enqueue(pending, a_generation);
		}
	}

	void ApplyPending(std::uint64_t a_generation) noexcept
	{
		auto& io = ImGui::GetIO();
		pendingCloseRequest = {};
		io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
		if (nativeGamepadGeneration.load(std::memory_order_acquire) == a_generation) {
			io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		}

		std::array<PendingEvent, queueCapacity> events;
		std::size_t                            count{};
		bool                                   overflowed{};
		{
			QueueLock lock{ queue.Lock };
			if (queue.Generation == a_generation) {
				count = queue.Count;
				overflowed = queue.Overflowed;
				std::copy_n(queue.Events.begin(), count, events.begin());
			}
			queue.Count = 0;
			queue.Overflowed = false;
			queue.Generation = a_generation;
		}

		if (overflowed) {
			io.ClearInputKeys();
			if (!overflowLogged.test_and_set(std::memory_order_relaxed)) {
				logger::error(
					"Native gamepad queue overflowed; cleared ImGui input state");
			}
			return;
		}
		for (std::size_t index = 0; index < count; ++index) {
			RequestCloseIfCancelHasNoTarget(events[index], a_generation);
			ApplyEvent(io, events[index]);
		}
	}

	RepeatTiming ApplyRepeatTiming() noexcept
	{
		auto& io = ImGui::GetIO();
		const RepeatTiming previous{ io.KeyRepeatDelay, io.KeyRepeatRate };
		const auto* context = ImGui::GetCurrentContext();
		if (context && context->NavInputSource == ImGuiInputSource_Gamepad) {
			io.KeyRepeatDelay = gamepadRepeatDelay;
			io.KeyRepeatRate = gamepadRepeatRate;
		}
		return previous;
	}

	void RestoreRepeatTiming(RepeatTiming a_timing) noexcept
	{
		auto& io = ImGui::GetIO();
		io.KeyRepeatDelay = a_timing.Delay;
		io.KeyRepeatRate = a_timing.Rate;
	}

	bool ConsumeCloseRequestForCurrentWindow(
		std::uint64_t a_generation) noexcept
	{
		if (pendingCloseRequest.Generation != a_generation) {
			return false;
		}
		const auto* current = ImGui::GetCurrentWindow();
		const auto* root = current ? current->RootWindowForNav : nullptr;
		if (!root || root->ID != pendingCloseRequest.WindowID) {
			return false;
		}
		pendingCloseRequest = {};
		return true;
	}
}
