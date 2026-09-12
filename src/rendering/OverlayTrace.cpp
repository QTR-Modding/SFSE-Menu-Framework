#include "rendering/OverlayTrace.h"
#include <array>
#include <atomic>
#include <Windows.h>

namespace SFSEMenuFramework::OverlayTrace
{
	namespace
	{
		std::array<std::atomic<unsigned>, Count> counts{};
		std::atomic<const char*> lastStateReason{ "none" };
		std::atomic<ULONGLONG> lastReport{};
		std::atomic<unsigned> reports{};
	}
	void Record(Event event) noexcept { counts[event].fetch_add(1, std::memory_order_relaxed); }
	void StateRejected(const char* reason) noexcept
	{
		Record(StateMissing);
		lastStateReason.store(reason, std::memory_order_relaxed);
	}
	void Report()
	{
		if (reports.load(std::memory_order_relaxed) >= 180) { return; }
		const auto now = GetTickCount64();
		auto previous = lastReport.load(std::memory_order_relaxed);
		if (now - previous < 2000 || !lastReport.compare_exchange_strong(previous, now)) { return; }
		reports.fetch_add(1, std::memory_order_relaxed);
		std::array<unsigned, Count> sample{};
		for (unsigned i = 0; i < Count; ++i) { sample[i] = counts[i].exchange(0, std::memory_order_relaxed); }
		logger::info("Overlay trace: tags={} null={} rejected={} state-missing={} ({}) duplicate={} "
			"imgui={} imgui-skipped={} present={} frame-ui={} frame-fallback={}",
			sample[Tag], sample[NullTag], sample[Rejected], sample[StateMissing],
			lastStateReason.load(std::memory_order_relaxed), sample[Duplicate], sample[ImGuiDraw],
			sample[ImGuiSkipped], sample[PresentDraw], sample[FrameUI], sample[FrameFallback]);
	}
}
