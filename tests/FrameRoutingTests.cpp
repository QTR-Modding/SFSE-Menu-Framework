#include "rendering/FrameRouteHistory.h"
#include "rendering/FramePresentBridge.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	void Check(bool value)
	{
		if (!value) { std::fputs("Frame routing test failed\n", stderr); std::abort(); }
	}

	void* seenSwapChain{};
	std::uint32_t seenSync{}, seenFlags{}, seenFrame{};
	std::int32_t __stdcall Capture(void* swapChain, std::uint32_t sync, std::uint32_t flags, std::uint32_t frame)
	{
		seenSwapChain = swapChain;
		seenSync = sync;
		seenFlags = flags;
		seenFrame = frame;
		return -123;
	}

	void CheckBridge()
	{
		using namespace SFSEMenuFramework;
		auto* memory = static_cast<std::byte*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		Check(memory != nullptr);
		// Synthetic caller preserves R15, places the packet there, then performs
		// the same six-byte CALL used at the verified game callsite.
		const std::uint8_t caller[]{
			0x41, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4D, 0x8B, 0xF9,
			0xFF, 0x15, 0, 0, 0, 0,
			0x48, 0x83, 0xC4, 0x20, 0x41, 0x5F, 0xC3
		};
		std::memcpy(memory, caller, sizeof(caller));
		const auto base = reinterpret_cast<std::uintptr_t>(memory);
		const FramePresentBridge bridge{ reinterpret_cast<std::uintptr_t>(&Capture) };
		std::memcpy(memory + 64, &bridge, sizeof(bridge));
		const auto target = base + 64;
		std::memcpy(memory + 96, &target, sizeof(target));
		const REL::ASM::CALL6 call{ base + 9, base + 96 };
		std::memcpy(memory + 9, &call, sizeof(call));
		DWORD previous{};
		Check(VirtualProtect(memory, 4096, PAGE_EXECUTE_READ, &previous) != FALSE);
		Check(FlushInstructionCache(GetCurrentProcess(), memory, 4096) != FALSE);
		using Runner = std::int32_t(__stdcall*)(void*, std::uint32_t, std::uint32_t, RE::CreationRendererPrivate::PresentRequest*);
		auto run = reinterpret_cast<Runner>(memory);
		RE::CreationRendererPrivate::PresentRequest request{};
		int sentinel{};
		for (std::uint32_t frame : { 0u, 17u, 0x80000000u, 0xFFFFFFFFu }) {
			request.frameIndex = frame;
			Check(run(&sentinel, 3, 0x128, &request) == -123);
			Check(seenSwapChain == &sentinel && seenSync == 3 && seenFlags == 0x128 && seenFrame == frame);
		}
		Check(VirtualFree(memory, 0, MEM_RELEASE) != FALSE);
	}
}

int main()
{
	using SFSEMenuFramework::FrameRouteHistory;
	FrameRouteHistory history;
	Check(!history.HasUI(0));
	history.Record(1, true);
	history.Record(2, false);
	history.Record(3, true);
	Check(history.HasUI(1)); // Later null tag must not suppress this queued frame.
	Check(history.HasUI(1)); // Multiple windows/retries retain the decision.
	Check(!history.HasUI(2)); // Later success must not suppress this frame's fallback.
	Check(history.HasUI(3));
	Check(!history.HasUI(4)); // No UI tag is not a recent-frame match.
	history.Record(5, true);
	history.Record(5, false);
	Check(!history.HasUI(5));
	for (std::uint32_t frame = 10; frame < 266; ++frame) { history.Record(frame, true); }
	for (std::uint32_t frame = 10; frame < 266; ++frame) { Check(history.HasUI(frame)); }
	Check(!history.HasUI(266));
	FrameRouteHistory wrap;
	wrap.Record(0xFFFFFFFEu, true);
	wrap.Record(0, true);
	Check(wrap.HasUI(0xFFFFFFFEu));
	Check(!wrap.HasUI(0xFFFFFFFFu));
	Check(wrap.HasUI(0));
	Check(!wrap.HasUI(1));
	CheckBridge();
	std::puts("Frame routing and native bridge tests passed");
}
