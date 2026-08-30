#pragma once

#include <array>
#include <cstdint>

#include <Windows.h>
#include <d3d12.h>

namespace SFSEMenuFramework::D3D12Renderer
{
	using SetDescriptorHeapsFunction = void(STDMETHODCALLTYPE*)(
		ID3D12GraphicsCommandList*,
		UINT,
		ID3D12DescriptorHeap* const*);

	struct DescriptorHeapSnapshot final
	{
		std::array<ID3D12DescriptorHeap*, 2> Heaps{};
		UINT                                 Count{ 0 };
	};

	enum class RenderResult : std::uint8_t
	{
		Rendered,
		InvalidArguments,
		Busy,
		DeviceQueryFailed,
		DeviceMismatch,
		CommandList2Unavailable,
		FrameSlotBusy,
		InvalidTarget,
		PlatformFrameUnavailable,
		InvalidDisplaySize,
		Count
	};

	[[nodiscard]] bool Initialize(ID3D12Device* a_device);
	void               SetPlatformInputEnabled(bool a_enabled);
	[[nodiscard]] bool HasRecentMainWindowFrame(std::uint64_t a_generation) noexcept;

	[[nodiscard]] RenderResult Render(
		ID3D12GraphicsCommandList*    a_commandList,
		ID3D12Resource*               a_renderTarget,
		const DescriptorHeapSnapshot& a_engineHeaps,
		SetDescriptorHeapsFunction    a_setDescriptorHeaps);
}
