#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

namespace SFSEMenuFramework::D3D12Textures
{
	struct Texture final
	{
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ViewHeap;
		Microsoft::WRL::ComPtr<ID3D12Resource> TextureResource;
		Microsoft::WRL::ComPtr<ID3D12Resource> UploadBuffer;

		void Reset() noexcept { *this = {}; }
	};

	[[nodiscard]] bool Upload(
		ID3D12Device*, ID3D12GraphicsCommandList*,
		const unsigned char* a_pixels, int a_width, int a_height, Texture&) noexcept;

	[[nodiscard]] D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE) noexcept;
	[[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(std::uint64_t) noexcept;
	[[nodiscard]] bool CreateDescriptorHeap(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE,
		D3D12_DESCRIPTOR_HEAP_FLAGS, Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>&,
		UINT a_count = 1) noexcept;
}
