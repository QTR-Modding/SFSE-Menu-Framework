#include "InputCapture.h"

#include "FrameworkSettings.h"
#include "WindowManager.h"

#include <RE/B/BSInputDeviceManagerInput.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>

namespace SFSEMenuFramework::InputCapture
{
	namespace
	{
		// The bounded event walk and verified vtable installation/rollback
		// pattern are adapted from QTR-Modding's
		// ToggleDialogueCameraSF at commit
		// 8021fa934591aac1c71266cc4abc5cb1c24e28d7. That project is
		// GPL-3.0-or-later with the Modding and GPL-3.0 Linking Exceptions.
		// The BSInputDeviceManager receiver ABI and dispatch placement were
		// independently verified against Starfield 1.16.244.
		// Preserving PrintScreen while modal is adapted from SKSE Menu
		// Framework 3's RemoveNonPrintScreenInputs at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775. This Starfield port
		// marks native events stopped instead of rewriting Skyrim's queue.
		constexpr std::size_t maximumInputEvents = 512;
		constexpr float       holdThresholdSeconds = 0.4F;
		constexpr auto        doublePressThreshold = std::chrono::milliseconds{ 300 };
		constexpr std::uint32_t initialKeyboardEdgeWindowMilliseconds = 250;
		constexpr std::uint32_t heldKeyboardEdgeWindowMilliseconds = 2000;
		constexpr std::uint32_t keyboardEdgeIDMask = 0xFF;
		constexpr std::uint32_t keyboardEdgeHeldBit = 1U << 8;
		constexpr std::uint32_t keyboardEdgeGenerationMask = 0x007FFFFF;
		constexpr std::array<std::uint8_t, 16> expectedInputProcessorPrologue{
			0x48, 0x85, 0xD2, 0x0F, 0x84, 0xAC, 0x01, 0x00,
			0x00, 0x53, 0x55, 0x48, 0x83, 0xEC, 0x38, 0x4C
		};

		enum class HookState : std::uint8_t
		{
			Uninitialized,
			Installing,
			Ready,
			Failed
		};

		using InputProcessor = RE::BSInputDeviceManagerInput::PerformInputProcessing_t*;

		std::atomic<HookState>      hookState{ HookState::Uninitialized };
		std::atomic<InputProcessor> originalInputProcessor{ nullptr };
		std::atomic<bool>           captureArmed{ false };
		std::atomic<bool>           modal{ false };
		std::atomic<std::uint64_t>  pendingKeyboardSuppression{ 0 };
		std::atomic<std::uint32_t>  keyboardEdgeGeneration{ 0 };
		std::atomic<bool>           captureFaulted{ false };
		std::atomic_flag            eventLimitLogged{};
		struct ToggleTracker final
		{
			std::chrono::steady_clock::time_point LastPress{};
			bool                                  HasLastPress{ false };
			bool                                  HoldTriggered{ false };
		};

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
			       button.idCode == VK_SNAPSHOT;
		}

		void FaultCaptureOnEventLimit() noexcept
		{
			captureFaulted.store(true, std::memory_order_release);
			modal.store(false, std::memory_order_release);
			pendingKeyboardSuppression.store(0, std::memory_order_release);
			WindowManager::CloseAllBlockingWindows();
			if (!eventLimitLogged.test_and_set(std::memory_order_relaxed)) {
				logger::critical(
					"BSInputDeviceManager input queue exceeded {} events; modal native capture was disabled",
					maximumInputEvents);
			}
		}

		[[nodiscard]] bool CaptureDeadlinePassed(
			std::uint32_t a_deadline,
			std::uint32_t a_now) noexcept
		{
			return static_cast<std::int32_t>(a_now - a_deadline) > 0;
		}

		void ExpirePendingKeyboardEdge() noexcept
		{
			auto pending = pendingKeyboardSuppression.load(std::memory_order_acquire);
			if (pending == 0) {
				return;
			}

			const auto deadline = static_cast<std::uint32_t>(pending >> 32);
			if (CaptureDeadlinePassed(deadline, ::GetTickCount())) {
				static_cast<void>(pendingKeyboardSuppression.compare_exchange_strong(
					pending,
					0,
					std::memory_order_acq_rel,
					std::memory_order_acquire));
			}
		}

		[[nodiscard]] bool TryClaimKeyboardSuppression(
			const RE::InputEvent* a_queueHead) noexcept
		{
			auto pending = pendingKeyboardSuppression.load(std::memory_order_acquire);
			if (pending == 0) {
				return false;
			}

			const auto deadline = static_cast<std::uint32_t>(pending >> 32);
			if (CaptureDeadlinePassed(deadline, ::GetTickCount())) {
				static_cast<void>(pendingKeyboardSuppression.compare_exchange_strong(
						pending,
						0,
						std::memory_order_acq_rel,
						std::memory_order_acquire));
				return false;
			}

			const auto token = static_cast<std::uint32_t>(pending);
			const auto expectedID =
				static_cast<std::int32_t>(token & keyboardEdgeIDMask);
			const bool requireHeld = (token & keyboardEdgeHeldBit) != 0;

			bool matches{};
			bool released{};
			bool keyboardObserved{};
			auto event = a_queueHead;
			std::size_t eventCount{};
			while (event && eventCount < maximumInputEvents) {
				if (event->eventType == RE::InputEvent::EventType::kButton) {
					const auto& button =
						static_cast<const RE::ButtonEvent&>(*event);
					if (button.deviceType ==
							RE::InputEvent::DeviceType::kKeyboard) {
						keyboardObserved = true;
						if (button.idCode == expectedID) {
							const bool isRelease = button.value == 0.0F;
							released = released || isRelease;
							matches = !isRelease &&
								(requireHeld ?
									 button.heldDownSecs > holdThresholdSeconds :
									 button.heldDownSecs == 0.0F);
							if (matches) {
								break;
							}
						}
					}
				}
				event = event->next;
				++eventCount;
			}

			if (!matches && (requireHeld ? !released : !keyboardObserved)) {
				// Held suppression may span unrelated and pre-threshold batches.
				// Initial suppression is eligible only in the first keyboard-bearing
				// manager batch after publication, preventing a later normalized-ID
				// collision from claiming it.
				return false;
			}

			if (!pendingKeyboardSuppression.compare_exchange_strong(
					pending,
					0,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				// A newer lossless edge owns the token and must be matched only by a
				// later engine batch.
				return false;
			}
			return matches;
		}

		void ForwardInput(
			RE::BSInputEventReceiver* a_receiver,
			const RE::InputEvent*     a_queueHead)
		{
			if (const auto original =
					originalInputProcessor.load(std::memory_order_acquire)) {
				original(a_receiver, a_queueHead);
			}
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

		[[nodiscard]] bool ProcessGamePadOpenClose(
			const RE::ButtonEvent& a_button) noexcept
		{
			if (a_button.status == RE::InputEvent::Status::kStop) {
				return false;
			}

			if (a_button.deviceType != RE::InputEvent::DeviceType::kGamepad) {
				return false;
			}
			if (!WindowManager::IsHotkeyEnabled()) {
				return false;
			}

			const auto mode = FrameworkSettings::GetToggleModeGamePad();
			const auto configured = FrameworkSettings::GetToggleKeyGamePad();
			const bool bindingMatches =
				configured != 0 && a_button.idCode >= 0 &&
				static_cast<std::uint32_t>(a_button.idCode) ==
					configured;
			if (!bindingMatches) {
				return false;
			}

			const auto* mainWindow = WindowManager::GetMainWindow();
			if (!mainWindow) {
				return false;
			}

			const bool initialPress = IsInitialPress(a_button);
			if (a_button.value == 0.0F) {
				gamePadToggle.HoldTriggered = false;
			}
			if (mainWindow->IsOpen.load(std::memory_order_acquire)) {
				return initialPress && WindowManager::SetMainWindowOpen(false);
			}

			if (mode == FrameworkSettings::ToggleMode::Off) {
				return false;
			}

			const bool toggle = EvaluateToggle(a_button, mode, gamePadToggle);
			return toggle && IsOperational() &&
			       WindowManager::SetMainWindowOpen(true);
		}

		void ProcessInput(
			RE::BSInputEventReceiver* a_receiver,
			const RE::InputEvent*      a_queueHead)
		{
			if (!captureArmed.load(std::memory_order_acquire)) {
				ForwardInput(a_receiver, a_queueHead);
				return;
			}

			ExpirePendingKeyboardEdge();
			const bool keyboardEdgeMatched =
				a_queueHead && TryClaimKeyboardSuppression(a_queueHead);

			bool stateChanged{};
			const bool captureBatch = modal.load(std::memory_order_acquire);
			if (captureBatch || IsOperational()) {
				auto event = a_queueHead;
				std::size_t eventCount{};
				while (event && eventCount < maximumInputEvents) {
					if (event->eventType == RE::InputEvent::EventType::kButton) {
						const auto& button =
							static_cast<const RE::ButtonEvent&>(*event);
						if (!keyboardEdgeMatched && !stateChanged) {
							stateChanged = ProcessGamePadOpenClose(button);
						}
					}
					event = event->next;
					++eventCount;
				}
				if (event) {
					// Partial capture is not a safe modal state. Fail open for future
					// batches and let the lifecycle owner close or suspend the menu.
					FaultCaptureOnEventLimit();
				} else if (captureBatch || keyboardEdgeMatched || stateChanged) {
					// SKSE Menu Framework sends an empty queue on an actual open/close
					// edge. During ordinary modal capture it retains only PrintScreen.
					// Marking the shared Starfield events stopped gives later receivers
					// the corresponding behavior without rewriting the linked queue.
					const bool preservePrintScreen =
						captureBatch && !keyboardEdgeMatched && !stateChanged;
					for (event = a_queueHead; event; event = event->next) {
						if (!preservePrintScreen || !IsPrintScreen(*event)) {
							auto* mutableEvent = const_cast<RE::InputEvent*>(event);
							mutableEvent->status = RE::InputEvent::Status::kStop;
						}
					}
				}
			}

			ForwardInput(a_receiver, a_queueHead);
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
			RE::BSInputDeviceManagerInput::BSINPUTEVENTRECEIVER_VTABLE
		};
		constexpr auto slot =
			RE::BSInputDeviceManagerInput::kPerformInputProcessingVFunc;
		const auto slotAddress = inputVtable.address() + sizeof(std::uintptr_t) * slot;
		if (!IsReadablePointer(slotAddress)) {
			logger::critical(
				"BSInputDeviceManager input hook preflight failed: unreadable vtable slot");
			hookState.store(HookState::Failed, std::memory_order_release);
			return false;
		}

		const auto originalAddress =
			*reinterpret_cast<const std::uintptr_t*>(slotAddress);
		if (!HasExpectedInputProcessorPrologue(originalAddress)) {
			logger::critical(
				"BSInputDeviceManager input hook preflight failed: unexpected Starfield 1.16.244 callback");
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
				"BSInputDeviceManager input hook verification failed (original matched {}, readback matched {}, rollback attempted {}, rollback verified {})",
				replacedAddress == originalAddress,
				liveAddress == hookAddress,
				rollbackAttempted,
				rollbackVerified);
			hookState.store(HookState::Failed, std::memory_order_release);
			return false;
		}

		hookState.store(HookState::Ready, std::memory_order_release);
		pendingKeyboardSuppression.store(0, std::memory_order_release);
		captureFaulted.store(false, std::memory_order_release);
		logger::info(
			"Installed BSInputDeviceManager global input capture at {:X}",
			originalAddress);
		return true;
	}

	void ArmFunctionalCapture() noexcept
	{
		captureArmed.store(true, std::memory_order_release);
	}

	void FlushDiagnostics() noexcept
	{
		ExpirePendingKeyboardEdge();
	}

	bool RequestKeyboardSuppression(
		std::int32_t      a_expectedEventID,
		KeyboardEdgeMatch a_match) noexcept
	{
		if (a_expectedEventID < 0 || a_expectedEventID > 0xFF) {
			return false;
		}
		const bool held = a_match == KeyboardEdgeMatch::HeldPress;
		const auto window = held ?
			heldKeyboardEdgeWindowMilliseconds :
			initialKeyboardEdgeWindowMilliseconds;
		const auto deadline =
			::GetTickCount() + window;
		const auto generation =
			(keyboardEdgeGeneration.fetch_add(1, std::memory_order_relaxed) + 1) &
			keyboardEdgeGenerationMask;
		const auto token =
			(generation << 9) |
			(held ? keyboardEdgeHeldBit : 0U) |
			static_cast<std::uint32_t>(a_expectedEventID);
		const auto pending =
			(static_cast<std::uint64_t>(deadline) << 32) | token;
		pendingKeyboardSuppression.store(pending, std::memory_order_release);
		return true;
	}

	void CancelPendingHeldKeyboardSuppression() noexcept
	{
		auto pending = pendingKeyboardSuppression.load(std::memory_order_acquire);
		while (pending != 0 &&
			(static_cast<std::uint32_t>(pending) & keyboardEdgeHeldBit) != 0) {
			if (pendingKeyboardSuppression.compare_exchange_weak(
					pending,
					0,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				return;
			}
		}
	}

	void CancelPendingKeyboardSuppression() noexcept
	{
		pendingKeyboardSuppression.store(0, std::memory_order_release);
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

	bool IsKeyboardEdgeOperational() noexcept
	{
		return IsOperational();
	}

	bool IsOperational() noexcept
	{
		return captureArmed.load(std::memory_order_acquire) &&
		       hookState.load(std::memory_order_acquire) == HookState::Ready &&
		       !captureFaulted.load(std::memory_order_acquire);
	}
}
