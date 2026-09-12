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

	[[nodiscard]] bool HasSameDeviceIdentity(
		ID3D12Device* a_left, ID3D12Device* a_right) noexcept;
	[[nodiscard]] bool Initialize(ID3D12Device* a_device);
	[[nodiscard]] bool SetPlatformInputEnabled(
		bool a_enabled,
		std::uint64_t a_earlyRawMouseGeneration = 0);
	[[nodiscard]] bool HasRecentBlockingWindowFrame(std::uint64_t a_generation) noexcept;

	bool Render(
		ID3D12GraphicsCommandList*    a_commandList,
		ID3D12Resource*               a_renderTarget,
		const DescriptorHeapSnapshot& a_engineHeaps,
		SetDescriptorHeapsFunction    a_setDescriptorHeaps);
}
