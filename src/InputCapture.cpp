#include "InputCapture.h"

#include "FrameworkSettings.h"
#include "WindowManager.h"

#include <RE/P/PlayerControlsManager.h>
#include <REX/W32/DINPUT.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>

namespace SFSEMenuFramework::InputCapture
{
	namespace
	{
		// The hook-site contract, bounded event walk, and verified vtable
		// installation/rollback pattern are adapted from QTR-Modding's
		// ToggleDialogueCameraSF at commit
		// 8021fa934591aac1c71266cc4abc5cb1c24e28d7. That project is
		// GPL-3.0-or-later with the Modding and GPL-3.0 Linking Exceptions.
		// Preserving PrintScreen while modal is adapted from SKSE Menu
		// Framework 3's RemoveNonPrintScreenInputs at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775. This Starfield port
		// marks native events stopped instead of rewriting Skyrim's queue.
		constexpr std::size_t maximumInputEvents = 512;
		constexpr float       holdThresholdSeconds = 0.4F;
		constexpr auto        doublePressThreshold = std::chrono::milliseconds{ 300 };
		constexpr std::array<std::uint8_t, 16> expectedInputProcessorPrologue{
			0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
			0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20
		};

		enum class HookState : std::uint8_t
		{
			Uninitialized,
			Installing,
			Ready,
			Failed
		};

		using InputProcessor = RE::PlayerControlsManager::PerformInputProcessing_t*;

		std::atomic<HookState>      hookState{ HookState::Uninitialized };
		std::atomic<InputProcessor> originalInputProcessor{ nullptr };
		std::atomic<bool>           modal{ false };
		std::atomic<bool>           captureFaulted{ false };
		std::atomic_flag            eventLimitLogged{};

		struct ToggleTracker final
		{
			std::chrono::steady_clock::time_point LastPress{};
			bool                                  HasLastPress{ false };
			bool                                  HoldTriggered{ false };
		};

		ToggleTracker keyboardToggle;
		ToggleTracker gamePadToggle;

		[[nodiscard]] bool IsReadablePointer(std::uintptr_t a_address) noexcept
		{
			if (!a_address || a_address % alignof(std::uintptr_t) != 0) {
				return false;
			}

			MEMORY_BASIC_INFORMATION memory{};
			if (::VirtualQuery(
					reinterpret_cast<const void*>(a_address),
					&memory,
					sizeof(memory)) == 0 ||
				memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 ||
				(memory.Protect & 0xFF) == PAGE_NOACCESS) {
				return false;
			}

			const auto regionEnd =
				reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
			return a_address + sizeof(std::uintptr_t) <= regionEnd;
		}

		[[nodiscard]] bool HasExpectedInputProcessorPrologue(
			std::uintptr_t a_address) noexcept
		{
			const auto text = REX::FModule::GetExecutingModule().GetSection(".text");
			const auto textAddress = text.GetAddress();
			const auto textSize = text.GetSize();
			if (!a_address || !textAddress || expectedInputProcessorPrologue.size() > textSize ||
				a_address < textAddress ||
				a_address - textAddress > textSize - expectedInputProcessorPrologue.size()) {
				return false;
			}

			return std::memcmp(
				reinterpret_cast<const void*>(a_address),
				expectedInputProcessorPrologue.data(),
				expectedInputProcessorPrologue.size()) == 0;
		}

		[[nodiscard]] bool IsPrintScreen(const RE::InputEvent& a_event) noexcept
		{
			if (a_event.eventType != RE::InputEvent::EventType::kButton) {
				return false;
			}

			const auto& button = static_cast<const RE::ButtonEvent&>(a_event);
			return button.deviceType == RE::InputEvent::DeviceType::kKeyboard &&
			       button.idCode == static_cast<std::int32_t>(REX::W32::DIK_SYSRQ);
		}

		[[nodiscard]] bool IsInitialPress(const RE::ButtonEvent& a_button) noexcept
		{
			return a_button.value != 0.0F && a_button.heldDownSecs == 0.0F;
		}

		[[nodiscard]] bool EvaluateToggle(
			const RE::ButtonEvent&             a_button,
			FrameworkSettings::ToggleMode      a_mode,
			ToggleTracker&                     a_tracker) noexcept
		{
			const bool initialPress = IsInitialPress(a_button);
			if (a_button.value == 0.0F) {
				a_tracker.HoldTriggered = false;
			}

			switch (a_mode) {
			case FrameworkSettings::ToggleMode::SinglePress:
				return initialPress;
			case FrameworkSettings::ToggleMode::Hold:
				if (a_button.value != 0.0F &&
					a_button.heldDownSecs > holdThresholdSeconds &&
					!a_tracker.HoldTriggered) {
					a_tracker.HoldTriggered = true;
					return true;
				}
				return false;
			case FrameworkSettings::ToggleMode::DoublePress:
				if (!initialPress) {
					return false;
				}
				{
					const auto now = std::chrono::steady_clock::now();
					const bool doublePress =
						a_tracker.HasLastPress &&
						now - a_tracker.LastPress < doublePressThreshold;
					a_tracker.LastPress = now;
					a_tracker.HasLastPress = !doublePress;
					return doublePress;
				}
			case FrameworkSettings::ToggleMode::Off:
			default:
				return false;
			}
		}

		[[nodiscard]] bool ProcessOpenClose(const RE::ButtonEvent& a_button) noexcept
		{
			if (a_button.status == RE::InputEvent::Status::kStop) {
				return false;
			}

			if (a_button.deviceType == RE::InputEvent::DeviceType::kKeyboard &&
				a_button.idCode == static_cast<std::int32_t>(REX::W32::DIK_ESCAPE) &&
				IsInitialPress(a_button)) {
				return WindowManager::SetMainWindowOpen(false);
			}

			FrameworkSettings::ToggleMode mode{};
			ToggleTracker* tracker{};
			bool bindingMatches{};
			switch (a_button.deviceType) {
			case RE::InputEvent::DeviceType::kKeyboard:
				mode = FrameworkSettings::GetToggleMode();
				tracker = &keyboardToggle;
				bindingMatches =
					a_button.idCode >= 0 &&
					static_cast<std::uint32_t>(a_button.idCode) ==
						FrameworkSettings::GetToggleKey();
				break;
			case RE::InputEvent::DeviceType::kGamepad:
				mode = FrameworkSettings::GetToggleModeGamePad();
				tracker = &gamePadToggle;
				bindingMatches =
					a_button.idCode >= 0 &&
					static_cast<std::uint32_t>(a_button.idCode) ==
						FrameworkSettings::GetToggleKeyGamePad();
				break;
			default:
				return false;
			}

			if (!bindingMatches || !tracker) {
				return false;
			}

			const auto* mainWindow = WindowManager::GetMainWindow();
			if (!mainWindow) {
				return false;
			}

			const bool initialPress = IsInitialPress(a_button);
			if (a_button.value == 0.0F) {
				tracker->HoldTriggered = false;
			}
			if (mainWindow->IsOpen.load(std::memory_order_acquire)) {
				return initialPress && WindowManager::SetMainWindowOpen(false);
			}

			if (mode == FrameworkSettings::ToggleMode::Off) {
				return false;
			}

			const bool toggle = EvaluateToggle(a_button, mode, *tracker);
			return toggle && IsOperational() &&
			       WindowManager::SetMainWindowOpen(true);
		}

		void ProcessInput(
			RE::BSInputEventReceiver* a_receiver,
			const RE::InputEvent*      a_queueHead)
		{
			const bool captureBatch = modal.load(std::memory_order_acquire);
			if (captureBatch || IsOperational()) {
				auto event = a_queueHead;
				std::size_t eventCount{};
				bool stateChanged{};
				while (event && eventCount < maximumInputEvents) {
					if (!stateChanged &&
						event->eventType == RE::InputEvent::EventType::kButton) {
						stateChanged = ProcessOpenClose(
							static_cast<const RE::ButtonEvent&>(*event));
					}
					event = event->next;
					++eventCount;
				}

				if (event) {
					// Partial capture is not a safe modal state. Fail open for future
					// batches and let the lifecycle owner close or suspend the menu.
					captureFaulted.store(true, std::memory_order_release);
					modal.store(false, std::memory_order_release);
					static_cast<void>(WindowManager::SetMainWindowOpen(false));
					if (!eventLimitLogged.test_and_set(std::memory_order_relaxed)) {
						logger::critical(
							"PlayerControls input queue exceeded {} events; modal native capture was disabled",
							maximumInputEvents);
					}
				} else if (captureBatch || stateChanged) {
					// SKSE Menu Framework discards the whole native batch on an actual
					// open/close edge. Marking the shared Starfield events stopped gives
					// later receivers the same behavior without rewriting the queue.
					for (event = a_queueHead; event; event = event->next) {
						// PrintScreen remains visible to Starfield's receiver chain. The
						// Win32 subclass must likewise keep OS/system messages chained.
						if (!IsPrintScreen(*event)) {
							auto* mutableEvent = const_cast<RE::InputEvent*>(event);
							mutableEvent->status = RE::InputEvent::Status::kStop;
						}
					}
				}
			}

			const auto original = originalInputProcessor.load(std::memory_order_acquire);
			if (original) {
				original(a_receiver, a_queueHead);
			}
		}
	}

	bool Install()
	{
		auto expectedState = HookState::Uninitialized;
		if (!hookState.compare_exchange_strong(
				expectedState,
				HookState::Installing,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			return expectedState == HookState::Ready;
		}

		REL::Relocation<std::uintptr_t> inputVtable{
			RE::PlayerControlsManager::BSINPUTEVENTRECEIVER_VTABLE
		};
		constexpr auto slot = RE::PlayerControlsManager::kPerformInputProcessingVFunc;
		const auto slotAddress = inputVtable.address() + sizeof(std::uintptr_t) * slot;
		if (!IsReadablePointer(slotAddress)) {
			logger::critical("PlayerControls input hook preflight failed: unreadable vtable slot");
			hookState.store(HookState::Failed, std::memory_order_release);
			return false;
		}

		const auto originalAddress =
			*reinterpret_cast<const std::uintptr_t*>(slotAddress);
		if (!HasExpectedInputProcessorPrologue(originalAddress)) {
			logger::critical(
				"PlayerControls input hook preflight failed: unexpected Starfield 1.16.244 callback");
			hookState.store(HookState::Failed, std::memory_order_release);
			return false;
		}

		const auto hookAddress =
			REX::UNRESTRICTED_CAST<std::uintptr_t>(&ProcessInput);
		originalInputProcessor.store(
			REX::UNRESTRICTED_CAST<InputProcessor>(originalAddress),
			std::memory_order_release);

		const auto replacedAddress = inputVtable.write_vfunc(slot, hookAddress);
		const auto chainAddress = replacedAddress ? replacedAddress : originalAddress;
		originalInputProcessor.store(
			REX::UNRESTRICTED_CAST<InputProcessor>(chainAddress),
			std::memory_order_release);

		const auto liveAddress =
			*reinterpret_cast<const std::uintptr_t*>(slotAddress);
		if (replacedAddress != originalAddress || liveAddress != hookAddress) {
			bool rollbackAttempted{};
			bool rollbackVerified{};
			if (liveAddress == hookAddress) {
				rollbackAttempted = true;
				static_cast<void>(inputVtable.write_vfunc(slot, chainAddress));
				rollbackVerified =
					*reinterpret_cast<const std::uintptr_t*>(slotAddress) == chainAddress;
			}

			modal.store(false, std::memory_order_release);
			if (rollbackVerified) {
				originalInputProcessor.store(nullptr, std::memory_order_release);
			}
			logger::critical(
				"PlayerControls input hook verification failed (original matched {}, readback matched {}, rollback attempted {}, rollback verified {})",
				replacedAddress == originalAddress,
				liveAddress == hookAddress,
				rollbackAttempted,
				rollbackVerified);
			hookState.store(HookState::Failed, std::memory_order_release);
			return false;
		}

		hookState.store(HookState::Ready, std::memory_order_release);
		captureFaulted.store(false, std::memory_order_release);
		logger::info(
			"Installed PlayerControls native input capture at {:X}",
			originalAddress);
		return true;
	}

	void SetModal(bool a_modal) noexcept
	{
		modal.store(
			a_modal && IsOperational(),
			std::memory_order_release);
	}

	bool IsModal() noexcept
	{
		return modal.load(std::memory_order_acquire);
	}

	bool IsOperational() noexcept
	{
		return hookState.load(std::memory_order_acquire) == HookState::Ready &&
		       !captureFaulted.load(std::memory_order_acquire);
	}
}
