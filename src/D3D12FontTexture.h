#pragma once

#include <d3d12.h>

#include <wrl/client.h>

namespace SFSEMenuFramework::D3D12FontTexture
{
	struct Resources final
	{
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> ShaderHeap;
		Microsoft::WRL::ComPtr<ID3D12Resource>       Texture;
		Microsoft::WRL::ComPtr<ID3D12Resource>       UploadBuffer;

		void Reset() noexcept
		{
			UploadBuffer.Reset();
			Texture.Reset();
			ShaderHeap.Reset();
		}
	};

	[[nodiscard]] bool Create(
		ID3D12Device*             a_device,
		ID3D12GraphicsCommandList* a_commandList,
		const unsigned char*      a_pixels,
		int                       a_width,
		int                       a_height,
		Resources&                a_result) noexcept;
}
