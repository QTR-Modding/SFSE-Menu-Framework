#include "input/GamepadNavigation.h"
#include "ui/McpWindow.h"

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
		constexpr float       stickDeadzone = 0.30F;
		constexpr float       gamepadRepeatDelay = 0.625F;
		constexpr float       gamepadRepeatRate = 0.125F;

		enum class EventKind : std::uint8_t
		{
			Button,
			LeftStick,
			RightStick
		};

		enum class SequenceOwner : std::uint8_t
		{
			None,
			ImGui,
			Suppressed
		};

		struct ButtonMapping final
		{
			std::uint32_t ID;
			ImGuiKey      Key;
		};

		constexpr std::array buttonMappings{
			ButtonMapping{ 1, ImGuiKey_GamepadDpadUp },
			ButtonMapping{ 2, ImGuiKey_GamepadDpadDown },
			ButtonMapping{ 4, ImGuiKey_GamepadDpadLeft },
			ButtonMapping{ 8, ImGuiKey_GamepadDpadRight },
			ButtonMapping{ 9, ImGuiKey_GamepadL2 },
			ButtonMapping{ 10, ImGuiKey_GamepadR2 },
			ButtonMapping{ 16, ImGuiKey_GamepadStart },
			ButtonMapping{ 32, ImGuiKey_GamepadBack },
			ButtonMapping{ 64, ImGuiKey_GamepadL3 },
			ButtonMapping{ 128, ImGuiKey_GamepadR3 },
			ButtonMapping{ 256, ImGuiKey_GamepadL1 },
			ButtonMapping{ 512, ImGuiKey_GamepadR1 },
			ButtonMapping{ 4096, ImGuiKey_GamepadFaceDown },
			ButtonMapping{ 8192, ImGuiKey_GamepadFaceRight },
			ButtonMapping{ 16384, ImGuiKey_GamepadFaceLeft },
			ButtonMapping{ 32768, ImGuiKey_GamepadFaceUp }
		};
		constexpr std::size_t invalidButtonIndex = buttonMappings.size();

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
			std::array<SequenceOwner, buttonMappings.size()> ButtonOwners{};
			SequenceOwner                          LeftStickOwner{};
			SequenceOwner                          RightStickOwner{};
			std::size_t                            Count{};
			std::uint64_t                          Generation{};
			bool                                   Overflowed{};
		};

		Queue                       queue;
		std::atomic<std::uint64_t> lastInputStamp{};
		std::atomic<std::uint64_t> nativeGamepadGeneration{};
		std::atomic_flag           overflowLogged{};
		CloseRequest               pendingCloseRequest;
		std::uint64_t              backSequenceGeneration{};
		bool                       suppressBackSequence{};

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

		[[nodiscard]] std::size_t FindButtonIndex(std::uint32_t a_id) noexcept
		{
			for (std::size_t index = 0; index < buttonMappings.size(); ++index) {
				if (buttonMappings[index].ID == a_id) {
					return index;
				}
			}
			return invalidButtonIndex;
		}

		[[nodiscard]] ImGuiKey MapButton(std::uint32_t a_id) noexcept
		{
			const auto index = FindButtonIndex(a_id);
			return index == invalidButtonIndex ?
				ImGuiKey_None : buttonMappings[index].Key;
		}

		void ResetSequenceOwners(Queue& a_queue) noexcept
		{
			a_queue.ButtonOwners.fill(SequenceOwner::None);
			a_queue.LeftStickOwner = SequenceOwner::None;
			a_queue.RightStickOwner = SequenceOwner::None;
		}

		void SetGeneration(Queue& a_queue, std::uint64_t a_generation) noexcept
		{
			if (a_queue.Generation == a_generation) {
				return;
			}
			a_queue.Generation = a_generation;
			a_queue.Count = 0;
			a_queue.Overflowed = false;
			ResetSequenceOwners(a_queue);
		}

		void EnqueueLocked(Queue& a_queue, const PendingEvent& a_event) noexcept
		{
			if (a_queue.Overflowed) {
				return;
			}
			if (a_queue.Count == a_queue.Events.size()) {
				a_queue.Count = 0;
				a_queue.Overflowed = true;
				return;
			}
			a_queue.Events[a_queue.Count++] = a_event;
		}

		void EnqueueSequence(
			const PendingEvent& a_event,
			std::uint64_t       a_generation,
			bool                a_sequenceActive,
			bool                a_consumerAllowsImGui) noexcept
		{
			QueueLock lock{ queue.Lock };
			SetGeneration(queue, a_generation);

			SequenceOwner* owner{};
			if (a_event.Kind == EventKind::Button) {
				const auto index = FindButtonIndex(a_event.ID);
				if (index == invalidButtonIndex) {
					return;
				}
				owner = &queue.ButtonOwners[index];
			} else {
				owner = a_event.Kind == EventKind::LeftStick ?
					&queue.LeftStickOwner : &queue.RightStickOwner;
			}

			bool send{};
			if (!a_sequenceActive) {
				send = *owner == SequenceOwner::ImGui;
				*owner = SequenceOwner::None;
			} else if ((a_event.Kind == EventKind::Button && a_event.InitialPress) ||
				*owner == SequenceOwner::None) {
				*owner = a_consumerAllowsImGui ?
					SequenceOwner::ImGui : SequenceOwner::Suppressed;
				send = *owner == SequenceOwner::ImGui;
			} else {
				// Once a sequence starts, keep its ImGui lifetime coherent even if
				// a client consumes a later held or release transition.
				send = *owner == SequenceOwner::ImGui;
			}

			if (send) {
				EnqueueLocked(queue, a_event);
			}
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
		bool                  a_consumerAllowsImGui) noexcept
	{
		if (a_generation == 0 ||
			a_event.deviceType != RE::InputEvent::DeviceType::kGamepad) {
			return;
		}

		PendingEvent pending;
		bool         active{};
		bool         sequenceActive{};
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
			sequenceActive = pending.X > 0.0F;
			if (FindButtonIndex(pending.ID) == invalidButtonIndex) {
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
			// Centre drift must neither navigate nor switch away from the mouse.
			// Queue a zero on return to neutral so held ImGui directions release.
			if (std::abs(pending.X) <= stickDeadzone) pending.X = 0.0F;
			if (std::abs(pending.Y) <= stickDeadzone) pending.Y = 0.0F;
			active = pending.X != 0.0F || pending.Y != 0.0F;
			sequenceActive = active;
		} else {
			return;
		}

		if (active) {
			ObserveGamepadActivity(a_generation);
		}
		EnqueueSequence(
			pending, a_generation, sequenceActive, a_consumerAllowsImGui);
	}

	void ApplyPending(std::uint64_t a_generation) noexcept
	{
		auto& io = ImGui::GetIO();
		pendingCloseRequest = {};
		if (backSequenceGeneration != a_generation) {
			backSequenceGeneration = a_generation;
			suppressBackSequence = false;
		}
		io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
		if (nativeGamepadGeneration.load(std::memory_order_acquire) == a_generation) {
			io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		}

		std::array<PendingEvent, queueCapacity> events;
		std::size_t                            count{};
		bool                                   overflowed{};
		{
			QueueLock lock{ queue.Lock };
			SetGeneration(queue, a_generation);
			count = queue.Count;
			overflowed = queue.Overflowed;
			std::copy_n(queue.Events.begin(), count, events.begin());
			queue.Count = 0;
			queue.Overflowed = false;
			if (overflowed) {
				ResetSequenceOwners(queue);
			}
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
			const auto& event = events[index];
			if (event.Kind == EventKind::Button && event.ID == 8192) {
				if (event.InitialPress) {
					suppressBackSequence = McpWindow::ConsumeGamepadBack();
				}
				// Native input includes held samples: keep a handled B out of
				// ImGui until release, not just on its initial press.
				if (suppressBackSequence) {
					if (event.X == 0.0F) suppressBackSequence = false;
					continue;
				}
			}
			RequestCloseIfCancelHasNoTarget(event, a_generation);
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

	CloseTargetSnapshot SnapshotCloseTarget(
		std::uint64_t a_generation) noexcept
	{
		if (a_generation == 0 ||
			pendingCloseRequest.Generation != a_generation) {
			return {};
		}
		const auto* target = ImGui::FindWindowByID(pendingCloseRequest.WindowID);
		return {
			.LastFrameActive = target ? target->LastFrameActive : -1,
			.Pending = true
		};
	}

	bool ConsumeCloseRequestForNewlyRenderedTarget(
		std::uint64_t       a_generation,
		CloseTargetSnapshot a_beforeRender) noexcept
	{
		if (a_generation == 0 || !a_beforeRender.Pending ||
			pendingCloseRequest.Generation != a_generation) {
			return false;
		}
		const auto* target = ImGui::FindWindowByID(pendingCloseRequest.WindowID);
		if (!target || target->LastFrameActive != ImGui::GetFrameCount() ||
			target->LastFrameActive == a_beforeRender.LastFrameActive) {
			return false;
		}
		pendingCloseRequest = {};
		return true;
	}
}
