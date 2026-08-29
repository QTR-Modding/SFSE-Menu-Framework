#pragma once

#include <array>

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

	[[nodiscard]] bool Initialize(ID3D12Device* a_device);

	[[nodiscard]] bool Render(
		ID3D12GraphicsCommandList*    a_commandList,
		ID3D12Resource*               a_renderTarget,
		const DescriptorHeapSnapshot& a_engineHeaps,
		SetDescriptorHeapsFunction    a_setDescriptorHeaps);
}
