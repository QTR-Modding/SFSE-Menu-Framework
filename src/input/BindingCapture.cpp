#include "input/BindingCapture.h"

#include "config/FrameworkSettings.h"

#include <RE/B/BSInputEventUser.h>
#include <REX/W32/DINPUT.h>

#include <atomic>
#include <bitset>
#include <cmath>
#include <mutex>
#include <new>

#include <Windows.h>

// The press/release state machine, active-device selection, and fresh raw
// cancellation behavior adapt QTR-Modding/SKSE-Menu-Framework-3's
// include/Input.h and src/Input.cpp from user-authored commits 5b269fa through
// e297bbd (GPL-3.0).
// Starfield keyboard bindings are captured from lossless Win32 DIK transitions;
// controller bindings use Starfield's native InputEvent queue. Atomic batch
// ownership and correlation tokens bridge those two target-specific paths.
namespace SFSEMenuFramework::BindingCapture
{
	namespace
	{
		constexpr std::uint32_t gamepadCancel = 8192;
		constexpr float activityThreshold = 0.35F;
		constexpr std::size_t keyboardEventCount = 0x100;

		struct Capture final
		{
			std::mutex        Mutex;
			std::atomic<State> Phase{ State::Idle };
			Device            Target{ Device::Keyboard };
			Device            PressedDevice{ Device::Keyboard };
			std::uint32_t     PressedKey{};
			std::bitset<keyboardEventCount> PendingKeyboardSequences{};
		};

		[[nodiscard]] Capture* GetCapture() noexcept
		{
			static auto* capture = new (std::nothrow) Capture();
			return capture;
		}

		std::atomic<Device> activeDevice{ Device::Keyboard };

		void TrackKeyboardSequence(
			Capture&     a_capture,
			std::int32_t a_engineEventID,
			bool         a_allowNew) noexcept
		{
			if (a_engineEventID < 0 ||
				static_cast<std::size_t>(a_engineEventID) >= keyboardEventCount) {
				return;
			}

			const auto index = static_cast<std::size_t>(a_engineEventID);
			if (!a_allowNew &&
				!a_capture.PendingKeyboardSequences.test(index)) {
				return;
			}
			a_capture.PendingKeyboardSequences.set(index);
		}

		void ClearKeyboardSequences(Capture& a_capture) noexcept
		{
			a_capture.PendingKeyboardSequences.reset();
		}

		[[nodiscard]] bool IsInitialPress(
			const RE::ButtonEvent& a_button) noexcept
		{
			return a_button.value != 0.0F && a_button.heldDownSecs == 0.0F;
		}

		[[nodiscard]] bool IsRelease(
			const RE::ButtonEvent& a_button) noexcept
		{
			return a_button.value == 0.0F;
		}

		void StartPress(
			Capture& a_capture, Device a_device, std::uint32_t a_key) noexcept
		{
			a_capture.PressedDevice = a_device;
			a_capture.PressedKey = a_key;
			a_capture.Phase.store(State::Pressed, std::memory_order_release);
		}
	}

	void Begin(Device a_device) noexcept
	{
		auto* capture = GetCapture();
		if (!capture) {
			return;
		}
		std::scoped_lock lock{ capture->Mutex };
		capture->Target = a_device;
		capture->PressedDevice = a_device;
		capture->PressedKey = unboundKey;
		capture->Phase.store(State::Waiting, std::memory_order_release);
	}

	void BeginConfirmation() noexcept
	{
		auto* capture = GetCapture();
		if (!capture) {
			return;
		}
		std::scoped_lock lock{ capture->Mutex };
		capture->PressedKey = unboundKey;
		capture->Phase.store(State::Confirming, std::memory_order_release);
	}

	void Acknowledge() noexcept
	{
		auto* capture = GetCapture();
		if (!capture) {
			return;
		}
		std::scoped_lock lock{ capture->Mutex };
		capture->PressedKey = unboundKey;
		capture->Phase.store(State::Idle, std::memory_order_release);
	}

	void Abort() noexcept
	{
		auto* capture = GetCapture();
		if (!capture) {
			return;
		}
		std::scoped_lock lock{ capture->Mutex };
		capture->PressedKey = unboundKey;
		ClearKeyboardSequences(*capture);
		capture->Phase.store(State::Idle, std::memory_order_release);
	}

	bool IsActive() noexcept
	{
		const auto* capture = GetCapture();
		return capture &&
			capture->Phase.load(std::memory_order_acquire) != State::Idle;
	}

	bool IsConfirming() noexcept
	{
		const auto* capture = GetCapture();
		return capture &&
			capture->Phase.load(std::memory_order_acquire) == State::Confirming;
	}

	Device GetActiveDevice() noexcept
	{
		return activeDevice.load(std::memory_order_acquire);
	}

	State Poll(std::uint32_t& a_key, Device a_displayedDevice) noexcept
	{
		auto* capture = GetCapture();
		if (!capture) {
			return State::Idle;
		}
		std::scoped_lock lock{ capture->Mutex };
		auto state = capture->Phase.load(std::memory_order_acquire);
		if (capture->Target != a_displayedDevice && state != State::Pressed) {
			state = State::Idle;
			capture->Phase.store(state, std::memory_order_release);
		}
		if (state == State::Complete || state == State::Cancelled) {
			a_key = capture->PressedKey;
			capture->Phase.store(State::Idle, std::memory_order_release);
		}
		return state;
	}

	void ProcessKeyboardTransition(
		std::uint32_t a_dik,
		std::int32_t  a_engineEventID,
		bool          a_down) noexcept
	{
		if (a_down) {
			activeDevice.store(Device::Keyboard, std::memory_order_release);
		}

		auto* capture = GetCapture();
		if (!capture) {
			return;
		}

		std::scoped_lock lock{ capture->Mutex };
		const auto state = capture->Phase.load(std::memory_order_acquire);
		if (state == State::Idle) {
			if (!a_down) {
				TrackKeyboardSequence(
					*capture, a_engineEventID, false);
			}
			return;
		}
		TrackKeyboardSequence(
			*capture, a_engineEventID, true);
		const bool cancel = a_dik == REX::W32::DIK_ESCAPE;
		if (state == State::Confirming) {
			if (cancel && a_down) {
				capture->PressedDevice = Device::Keyboard;
				capture->PressedKey = a_dik;
			} else if (cancel && !a_down &&
				capture->PressedDevice == Device::Keyboard &&
				capture->PressedKey == a_dik) {
				capture->Phase.store(State::Cancelled, std::memory_order_release);
			}
			return;
		}
		if (state == State::Complete || state == State::Cancelled) {
			return;
		}

		if (a_down && (cancel ||
			(state == State::Waiting && capture->Target == Device::Keyboard &&
			 !FrameworkSettings::GetKeyboardBindingName(a_dik).empty()))) {
			StartPress(*capture, Device::Keyboard, a_dik);
		} else if (!a_down && state == State::Pressed &&
			capture->PressedDevice == Device::Keyboard &&
			capture->PressedKey == a_dik) {
			capture->Phase.store(
				cancel ? State::Cancelled : State::Complete,
				std::memory_order_release);
		}
	}

	void ObserveMouseActivity() noexcept
	{
		activeDevice.store(Device::Keyboard, std::memory_order_release);
	}

	NativeBatchResult ProcessNativeBatch(
		const RE::InputEvent* a_head, std::size_t a_limit) noexcept
	{
		auto* capture = GetCapture();
		if (!capture) {
			return {};
		}
		std::scoped_lock lock{ capture->Mutex };
		const bool captureActive =
			capture->Phase.load(std::memory_order_acquire) != State::Idle;
		NativeBatchResult result{ .OwnsBatch = captureActive };

		auto event = a_head;
		std::size_t eventCount{};
		while (event && eventCount < a_limit) {
			if (event->eventType == RE::InputEvent::EventType::kMouseMove) {
				activeDevice.store(Device::Keyboard, std::memory_order_release);
			} else if (event->eventType ==
				RE::InputEvent::EventType::kThumbstick) {
				const auto& stick =
					static_cast<const RE::ThumbstickEvent&>(*event);
				if (std::abs(stick.xValue) > activityThreshold ||
					std::abs(stick.yValue) > activityThreshold) {
					activeDevice.store(
						Device::Gamepad, std::memory_order_release);
				}
			} else if (event->eventType ==
				RE::InputEvent::EventType::kButton) {
				const auto& button =
					static_cast<const RE::ButtonEvent&>(*event);
				const bool initialPress = IsInitialPress(button);
				if (initialPress) {
					if (button.deviceType ==
						RE::InputEvent::DeviceType::kGamepad) {
						activeDevice.store(
							Device::Gamepad, std::memory_order_release);
					} else if (button.deviceType ==
							RE::InputEvent::DeviceType::kKeyboard ||
						button.deviceType ==
							RE::InputEvent::DeviceType::kMouse) {
						activeDevice.store(
							Device::Keyboard, std::memory_order_release);
					}
				}

				if (button.deviceType ==
						RE::InputEvent::DeviceType::kKeyboard &&
					button.idCode >= 0 &&
					static_cast<std::size_t>(button.idCode) <
						keyboardEventCount) {
					const auto index =
						static_cast<std::size_t>(button.idCode);
					if (capture->PendingKeyboardSequences.test(index)) {
						result.OwnsBatch = true;
						if (IsRelease(button)) {
							capture->PendingKeyboardSequences.reset(index);
						}
					}
				}

				const auto state =
					capture->Phase.load(std::memory_order_acquire);
				if (button.deviceType ==
						RE::InputEvent::DeviceType::kGamepad &&
					button.idCode >= 0 && state != State::Idle &&
					state != State::Complete &&
					state != State::Cancelled) {
					const auto key =
						static_cast<std::uint32_t>(button.idCode);
					if (state == State::Confirming) {
						if (key == gamepadCancel) {
							result.SuppressGamepadCancel = true;
						}
						if (key == gamepadCancel && initialPress) {
							capture->PressedDevice = Device::Gamepad;
							capture->PressedKey = key;
						} else if (key == gamepadCancel &&
							IsRelease(button) &&
							capture->PressedDevice == Device::Gamepad &&
							capture->PressedKey == key) {
							capture->Phase.store(
								State::Cancelled, std::memory_order_release);
						}
					} else if (initialPress && state == State::Waiting &&
						capture->Target == Device::Gamepad &&
						key != unboundKey &&
						!FrameworkSettings::GetGamePadBindingName(key).empty()) {
						StartPress(*capture, Device::Gamepad, key);
					} else if (IsRelease(button) &&
						state == State::Pressed &&
						capture->PressedDevice == Device::Gamepad &&
						capture->PressedKey == key) {
						capture->Phase.store(
							State::Complete, std::memory_order_release);
					}
				}
			}
			event = event->next;
			++eventCount;
		}
		result.ForwardAllToImGui =
			capture->Phase.load(std::memory_order_acquire) ==
				State::Confirming;
		result.Overflowed = event != nullptr;
		return result;
	}

	bool ShouldForwardKeyboardMessage(
		std::uint32_t a_virtualKey, bool a_down) noexcept
	{
		if (!IsActive()) {
			return true;
		}
		return a_virtualKey != VK_ESCAPE &&
			(IsConfirming() || !a_down);
	}
}
