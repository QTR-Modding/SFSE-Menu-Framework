#include "InputCapture.h"

#include "FrameworkSettings.h"
#include "WindowManager.h"

#include <RE/B/BSInputDeviceManagerInput.h>
#include <REX/W32/DINPUT.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <utility>

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

		enum class DiagnosticEdge : std::uint8_t
		{
			None,
			Opened,
			Closed
		};

		enum class DiagnosticTraceState : std::uint8_t
		{
			Idle,
			Preparing,
			Armed,
			Writing,
			Ready,
			Reporting,
			Completed
		};

		struct DiagnosticSample final
		{
			std::uint32_t EventIndex{ 0 };
			std::uint32_t EventType{ 0 };
			std::uint32_t DeviceType{ 0 };
			std::uint32_t DeviceID{ 0 };
			std::int32_t  IDCode{ -1 };
			std::uint32_t TimeCode{ 0 };
			std::uint32_t Status{ 0 };
			std::uint32_t ValueBits{ 0 };
			std::uint32_t HeldDownBits{ 0 };
		};

		using InputProcessor = RE::BSInputDeviceManagerInput::PerformInputProcessing_t*;

		std::atomic<HookState>      hookState{ HookState::Uninitialized };
		std::atomic<InputProcessor> originalInputProcessor{ nullptr };
		std::atomic<bool>           modal{ false };
		std::atomic<bool>           captureFaulted{ false };
		std::atomic_flag            inputBatchObserved{};
		std::atomic<bool>           inputBatchReportPending{ false };
		std::atomic<DiagnosticEdge> edgeReportPending{ DiagnosticEdge::None };
		std::atomic_flag            eventLimitLogged{};
		std::atomic<DiagnosticTraceState> diagnosticTraceState{
			DiagnosticTraceState::Idle
		};
		DiagnosticSample diagnosticSample{};

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

		void RecordKeyboardDiagnostic(
			const RE::ButtonEvent& a_button,
			std::size_t            a_eventIndex) noexcept
		{
			if (a_button.deviceType != RE::InputEvent::DeviceType::kKeyboard) {
				return;
			}

			auto expected = DiagnosticTraceState::Armed;
			if (!diagnosticTraceState.compare_exchange_strong(
					expected,
					DiagnosticTraceState::Writing,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				return;
			}

			diagnosticSample = {
				.EventIndex = static_cast<std::uint32_t>(a_eventIndex),
				.EventType = std::to_underlying(a_button.eventType),
				.DeviceType = std::to_underlying(a_button.deviceType),
				.DeviceID = a_button.deviceID,
				.IDCode = a_button.idCode,
				.TimeCode = a_button.timeCode,
				.Status = std::to_underlying(a_button.status),
				.ValueBits = std::bit_cast<std::uint32_t>(a_button.value),
				.HeldDownBits = std::bit_cast<std::uint32_t>(a_button.heldDownSecs),
			};
			diagnosticTraceState.store(
				DiagnosticTraceState::Ready,
				std::memory_order_release);
		}

		void ProcessInput(
			RE::BSInputEventReceiver* a_receiver,
			const RE::InputEvent*      a_queueHead)
		{
			if (a_queueHead &&
				!inputBatchObserved.test_and_set(std::memory_order_relaxed)) {
				inputBatchReportPending.store(true, std::memory_order_release);
			}

			const bool captureBatch = modal.load(std::memory_order_acquire);
			if (captureBatch || IsOperational()) {
				auto event = a_queueHead;
				std::size_t eventCount{};
				bool stateChanged{};
				while (event && eventCount < maximumInputEvents) {
					if (event->eventType == RE::InputEvent::EventType::kButton) {
						const auto& button =
							static_cast<const RE::ButtonEvent&>(*event);
						RecordKeyboardDiagnostic(button, eventCount);
						if (!stateChanged) {
							stateChanged = ProcessOpenClose(button);
						}
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
							"BSInputDeviceManager input queue exceeded {} events; modal native capture was disabled",
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

					if (stateChanged) {
						const auto* mainWindow = WindowManager::GetMainWindow();
						const bool isOpen =
							mainWindow &&
							mainWindow->IsOpen.load(std::memory_order_acquire);
						edgeReportPending.store(
							isOpen ? DiagnosticEdge::Opened : DiagnosticEdge::Closed,
							std::memory_order_release);
					}
				}
			}

			const auto original = originalInputProcessor.load(std::memory_order_acquire);
			if (original) {
				original(a_receiver, a_queueHead);
			}
		}
	}

	void ArmKeyboardDiagnosticTrace() noexcept
	{
		auto expected = DiagnosticTraceState::Idle;
		if (!diagnosticTraceState.compare_exchange_strong(
				expected,
				DiagnosticTraceState::Preparing,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			return;
		}

		diagnosticSample = {};
		diagnosticTraceState.store(
			DiagnosticTraceState::Armed,
			std::memory_order_release);
		logger::info(
			"Input primitive trace armed for the first keyboard ButtonEvent");
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
		captureFaulted.store(false, std::memory_order_release);
		logger::info(
			"Installed BSInputDeviceManager global input capture at {:X}",
			originalAddress);
		return true;
	}

	void FlushDiagnostics() noexcept
	{
		if (inputBatchReportPending.exchange(false, std::memory_order_acq_rel)) {
			logger::info(
				"BSInputDeviceManager input receiver observed its first non-empty batch");
		}

		const auto edge = edgeReportPending.exchange(
			DiagnosticEdge::None,
			std::memory_order_acq_rel);
		if (edge != DiagnosticEdge::None) {
			logger::info(
				"BSInputDeviceManager input receiver {} the Mod Control Panel",
				edge == DiagnosticEdge::Opened ? "opened" : "closed");
		}

		auto expected = DiagnosticTraceState::Ready;
		if (!diagnosticTraceState.compare_exchange_strong(
				expected,
				DiagnosticTraceState::Reporting,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			return;
		}

		const auto sample = diagnosticSample;
		const auto value = std::bit_cast<float>(sample.ValueBits);
		const auto heldDown = std::bit_cast<float>(sample.HeldDownBits);
		const bool bindingMatches = sample.IDCode >= 0 &&
			static_cast<std::uint32_t>(sample.IDCode) ==
				FrameworkSettings::GetToggleKey();
		const bool initialPress = value != 0.0F && heldDown == 0.0F;
		logger::info(
			"Input primitive trace: event={}, type={}, device={}, device-id={}, id={} (0x{:X}), time={}, status={}, value={} (0x{:08X}), held={} (0x{:08X}), configured DIK={}, mode={}, binding-match={}, initial-press={}",
			sample.EventIndex,
			sample.EventType,
			sample.DeviceType,
			sample.DeviceID,
			sample.IDCode,
			static_cast<std::uint32_t>(sample.IDCode),
			sample.TimeCode,
			sample.Status,
			value,
			sample.ValueBits,
			heldDown,
			sample.HeldDownBits,
			FrameworkSettings::GetToggleKey(),
			std::to_underlying(FrameworkSettings::GetToggleMode()),
			bindingMatches,
			initialPress);
		diagnosticTraceState.store(
			DiagnosticTraceState::Completed,
			std::memory_order_release);
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
