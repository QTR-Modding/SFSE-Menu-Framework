#include "LifecycleProbe.h"

#include "Win32Platform.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace SFSEMenuFramework::LifecycleProbe
{
	namespace
	{
		constexpr std::uint8_t postDataLoadBoundary = 1U << 0;
		constexpr std::uint8_t firstInputBoundary = 1U << 1;
		constexpr std::uint8_t firstInputBeforePostDataLoad = 1U << 2;

		std::atomic<std::uint8_t> lifecycleBoundaryState{ 0 };
		std::atomic<ULONGLONG> postDataLoadTick{ 0 };
		std::atomic<DWORD> postDataLoadThreadID{ 0 };
		std::atomic<bool> postDataLoadSampleReady{ false };
		std::atomic<bool> firstInputCallbackQueueNull{ true };
		std::atomic<ULONGLONG> firstInputCallbackTick{ 0 };
		std::atomic<DWORD> firstInputCallbackThreadID{ 0 };
		std::atomic<bool> firstInputCallbackReportPending{ false };
	}

	void RecordInputCallback(bool a_nonEmptyQueue) noexcept
	{
		auto boundaryState = lifecycleBoundaryState.load(std::memory_order_acquire);
		for (;;) {
			if ((boundaryState & firstInputBoundary) != 0) {
				return;
			}

			auto desiredState = static_cast<std::uint8_t>(
				boundaryState | firstInputBoundary);
			if ((boundaryState & postDataLoadBoundary) == 0) {
				desiredState = static_cast<std::uint8_t>(
					desiredState | firstInputBeforePostDataLoad);
			}

			if (lifecycleBoundaryState.compare_exchange_weak(
					boundaryState,
					desiredState,
					std::memory_order_acq_rel,
					std::memory_order_acquire)) {
				break;
			}
		}

		firstInputCallbackTick.store(::GetTickCount64(), std::memory_order_relaxed);
		firstInputCallbackThreadID.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		firstInputCallbackQueueNull.store(!a_nonEmptyQueue, std::memory_order_relaxed);
		firstInputCallbackReportPending.store(true, std::memory_order_release);
	}

	void RecordPostDataLoad() noexcept
	{
		const auto previousState = lifecycleBoundaryState.fetch_or(
			postDataLoadBoundary,
			std::memory_order_acq_rel);
		if ((previousState & postDataLoadBoundary) != 0) {
			return;
		}

		const auto markerTick = ::GetTickCount64();
		const auto markerThreadID = ::GetCurrentThreadId();
		postDataLoadTick.store(markerTick, std::memory_order_relaxed);
		postDataLoadThreadID.store(markerThreadID, std::memory_order_relaxed);
		postDataLoadSampleReady.store(true, std::memory_order_release);
		logger::info(
			"Lifecycle probe: SFSE post-data-load boundary at tick {}, thread {} (first input boundary already observed {})",
			markerTick,
			markerThreadID,
			(previousState & firstInputBoundary) != 0);
	}

	void Flush() noexcept
	{
		if (!Win32Platform::IsCurrentThreadHostWindowThread()) {
			return;
		}
		if (!postDataLoadSampleReady.load(std::memory_order_acquire)) {
			return;
		}

		if (!firstInputCallbackReportPending.exchange(
				false,
				std::memory_order_acq_rel)) {
			return;
		}

		const auto boundaryState =
			lifecycleBoundaryState.load(std::memory_order_acquire);
		const bool beforePostDataLoad =
			(boundaryState & firstInputBeforePostDataLoad) != 0;
		const bool queueNull =
			firstInputCallbackQueueNull.load(std::memory_order_relaxed);
		const auto callbackTick =
			firstInputCallbackTick.load(std::memory_order_relaxed);
		const auto callbackThreadID =
			firstInputCallbackThreadID.load(std::memory_order_relaxed);
		const auto markerTick = postDataLoadTick.load(std::memory_order_relaxed);
		const auto markerThreadID = postDataLoadThreadID.load(std::memory_order_relaxed);
		const auto diagnosticTick = ::GetTickCount64();
		const auto verifiedHostWindowThreadID = ::GetCurrentThreadId();

		logger::info(
			"Lifecycle probe: first BSInputDeviceManager callback boundary {} SFSE post-data-load; callback sample tick {}, thread {}, queue {}; post-data-load sample tick {}, thread {}; diagnostic tick {}, verified Starfield HWND thread {}",
			beforePostDataLoad ? "preceded" : "followed",
			callbackTick,
			callbackThreadID,
			queueNull ? "null" : "non-null",
			markerTick,
			markerThreadID,
			diagnosticTick,
			verifiedHostWindowThreadID);
	}
}
