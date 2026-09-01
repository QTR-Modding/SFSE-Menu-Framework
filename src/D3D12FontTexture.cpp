#include "D3D12FontTexture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace SFSEMenuFramework::D3D12FontTexture
{
	namespace
	{
		// The resource creation and upload sequence adapts Dear ImGui 1.90.8
		// backends/imgui_impl_dx12.cpp at commit
		// 6f7b5d0ee2fe9948ab871a530888a6dc5c960700 (MIT). This version adds
		// complete result checks and creates a new descriptor heap per generation
		// so an in-flight Starfield command list never observes a rewritten SRV.
		// The copy is recorded into Starfield's current direct command list; the
		// renderer's completion slot retains the upload buffer until that frame's
		// GPU marker completes, avoiding a blocking CPU wait or private queue.

		[[nodiscard]] bool CheckResult(HRESULT a_result, const char* a_operation) noexcept
		{
			if (SUCCEEDED(a_result)) {
				return true;
			}

			logger::error(
				"Live font texture {} failed with HRESULT 0x{:08X}",
				a_operation,
				static_cast<std::uint32_t>(a_result));
			return false;
		}
	}

	bool Create(
		ID3D12Device*             a_device,
		ID3D12GraphicsCommandList* a_commandList,
		const unsigned char*      a_pixels,
		int                       a_width,
		int                       a_height,
		Resources&                a_result) noexcept
	{
		a_result.Reset();
		if (!a_device || !a_commandList ||
			a_commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
			!a_pixels || a_width <= 0 || a_height <= 0 ||
			a_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			a_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
			logger::error(
				"Rejected invalid live font texture dimensions {}x{}",
				a_width,
				a_height);
			return false;
		}

		const auto rowBytes = static_cast<std::uint64_t>(a_width) * 4;
		const auto uploadPitch =
			(rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
			~static_cast<std::uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		const auto uploadSize = uploadPitch * static_cast<std::uint64_t>(a_height);
		if (uploadPitch > (std::numeric_limits<UINT>::max)() ||
			uploadSize == 0 ||
			uploadSize > (std::numeric_limits<std::size_t>::max)()) {
			logger::error("Live font texture upload dimensions overflowed");
			return false;
		}

		Resources candidate;
		D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
		heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDescription.NumDescriptors = 1;
		heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (!CheckResult(
				a_device->CreateDescriptorHeap(
					&heapDescription,
					IID_PPV_ARGS(candidate.ShaderHeap.GetAddressOf())),
				"descriptor-heap creation")) {
			return false;
		}
		if (candidate.ShaderHeap->GetGPUDescriptorHandleForHeapStart().ptr == 0) {
			logger::error("Live font texture descriptor heap returned a null GPU handle");
			return false;
		}

		D3D12_HEAP_PROPERTIES defaultHeapProperties{};
		defaultHeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
		defaultHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		defaultHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		defaultHeapProperties.CreationNodeMask = 1;
		defaultHeapProperties.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC textureDescription{};
		textureDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		textureDescription.Width = static_cast<UINT64>(a_width);
		textureDescription.Height = static_cast<UINT>(a_height);
		textureDescription.DepthOrArraySize = 1;
		textureDescription.MipLevels = 1;
		textureDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDescription.SampleDesc.Count = 1;
		textureDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		textureDescription.Flags = D3D12_RESOURCE_FLAG_NONE;
		if (!CheckResult(
				a_device->CreateCommittedResource(
					&defaultHeapProperties,
					D3D12_HEAP_FLAG_NONE,
					&textureDescription,
					D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr,
					IID_PPV_ARGS(candidate.Texture.GetAddressOf())),
				"texture creation")) {
			return false;
		}

		D3D12_HEAP_PROPERTIES uploadHeapProperties{};
		uploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
		uploadHeapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		uploadHeapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		uploadHeapProperties.CreationNodeMask = 1;
		uploadHeapProperties.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC uploadDescription{};
		uploadDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		uploadDescription.Width = uploadSize;
		uploadDescription.Height = 1;
		uploadDescription.DepthOrArraySize = 1;
		uploadDescription.MipLevels = 1;
		uploadDescription.Format = DXGI_FORMAT_UNKNOWN;
		uploadDescription.SampleDesc.Count = 1;
		uploadDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		uploadDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

		if (!CheckResult(
				a_device->CreateCommittedResource(
					&uploadHeapProperties,
					D3D12_HEAP_FLAG_NONE,
					&uploadDescription,
					D3D12_RESOURCE_STATE_GENERIC_READ,
					nullptr,
					IID_PPV_ARGS(candidate.UploadBuffer.GetAddressOf())),
				"upload-buffer creation")) {
			return false;
		}

		void* mapped{};
		constexpr D3D12_RANGE noCpuReads{ 0, 0 };
		if (!CheckResult(
				candidate.UploadBuffer->Map(0, &noCpuReads, &mapped),
				"upload-buffer map") ||
			!mapped) {
			return false;
		}
		for (int row = 0; row < a_height; ++row) {
			std::memcpy(
				static_cast<std::byte*>(mapped) +
					static_cast<std::size_t>(row) * static_cast<std::size_t>(uploadPitch),
				a_pixels + static_cast<std::size_t>(row) *
					static_cast<std::size_t>(rowBytes),
				static_cast<std::size_t>(rowBytes));
		}
		const D3D12_RANGE cpuWrites{
			0,
			static_cast<SIZE_T>(uploadSize)
		};
		candidate.UploadBuffer->Unmap(0, &cpuWrites);

		D3D12_TEXTURE_COPY_LOCATION source{};
		source.pResource = candidate.UploadBuffer.Get();
		source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		source.PlacedFootprint.Footprint.Width = static_cast<UINT>(a_width);
		source.PlacedFootprint.Footprint.Height = static_cast<UINT>(a_height);
		source.PlacedFootprint.Footprint.Depth = 1;
		source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(uploadPitch);

		D3D12_TEXTURE_COPY_LOCATION destination{};
		destination.pResource = candidate.Texture.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		destination.SubresourceIndex = 0;
		a_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = candidate.Texture.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		a_commandList->ResourceBarrier(1, &barrier);

		D3D12_SHADER_RESOURCE_VIEW_DESC shaderView{};
		shaderView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		shaderView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		shaderView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		shaderView.Texture2D.MostDetailedMip = 0;
		shaderView.Texture2D.MipLevels = 1;
		a_device->CreateShaderResourceView(
			candidate.Texture.Get(),
			&shaderView,
			candidate.ShaderHeap->GetCPUDescriptorHandleForHeapStart());

		a_result = std::move(candidate);
		return true;
	}
}
