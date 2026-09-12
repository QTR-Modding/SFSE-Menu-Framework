#pragma once

#include <cstdint>

#include <Windows.h>
#include <d3d12.h>

namespace SFSEMenuFramework::D3D12Renderer
{
	[[nodiscard]] bool HasSameIdentity(IUnknown* a_left, IUnknown* a_right) noexcept;
	[[nodiscard]] bool Initialize(ID3D12Device* a_device);
	[[nodiscard]] bool SetPlatformInputEnabled(
		bool a_enabled,
		std::uint64_t a_earlyRawMouseGeneration = 0);
	[[nodiscard]] bool HasRecentBlockingWindowFrame(std::uint64_t a_generation) noexcept;
	void NotifyOverlayComposited(std::uint64_t a_generation) noexcept;

	// A skipped draw leaves the cached generation unchanged. Zero means HUD-only.
	// The caller owns command-list state restoration and reports compositing separately.
	bool Render(ID3D12GraphicsCommandList* a_commandList, ID3D12Resource* a_renderTarget,
		std::uint64_t& a_generation);
}
