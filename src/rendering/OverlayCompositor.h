#pragma once

#include <cstdint>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace SFSEMenuFramework::OverlayCompositor
{
	struct Extent
	{
		std::uint32_t Top, Left, Width, Height;
		// An entirely zero extent selects the full texture. Reject malformed or out-of-bounds regions.
		bool Resolve(std::uint64_t textureWidth, std::uint32_t textureHeight, D3D12_RECT& rect) const noexcept;
	};

	struct Shaders
	{
		Microsoft::WRL::ComPtr<ID3D12RootSignature> RootSignature;
		Microsoft::WRL::ComPtr<ID3DBlob> VS;
		Microsoft::WRL::ComPtr<ID3DBlob> PS;
	};

	DXGI_FORMAT RtvFormat(DXGI_FORMAT format) noexcept;
	bool CreateShaders(ID3D12Device* device, Shaders& shaders);
	bool CreatePipeline(ID3D12Device* device, const Shaders& shaders, DXGI_FORMAT a_format,
		Microsoft::WRL::ComPtr<ID3D12PipelineState>& pipeline);
	// Callers own synchronization and retain replaced textures/heaps until GPU completion.
	bool CreateTexture(ID3D12Device* device, std::uint64_t a_width, std::uint32_t a_height,
		ID3D12DescriptorHeap* srvHeap,
		Microsoft::WRL::ComPtr<ID3D12Resource>& overlay);
	void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept;
	void Draw(ID3D12GraphicsCommandList* list, const Shaders& shaders, ID3D12PipelineState* pipeline,
		ID3D12DescriptorHeap* heap, D3D12_CPU_DESCRIPTOR_HANDLE targetRtv, const D3D12_RECT& region);
} // namespace SFSEMenuFramework::OverlayCompositor
