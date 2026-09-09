#include "rendering/D3D12Texture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace SFSEMenuFramework::D3D12Textures
{
	using Microsoft::WRL::ComPtr;
	[[nodiscard]] bool CheckResult(HRESULT a_result, const char* a_operation) noexcept
	{
		if (SUCCEEDED(a_result)) {
			return true;
		}
		logger::error(
			"D3D12 {} failed with HRESULT 0x{:08X}",
			a_operation,
			static_cast<std::uint32_t>(a_result));
		return false;
	}

	[[nodiscard]] D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE a_type) noexcept
	{
		D3D12_HEAP_PROPERTIES result{};
		result.Type = a_type;
		result.CreationNodeMask = 1;
		result.VisibleNodeMask = 1;
		return result;
	}

	[[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(std::uint64_t a_size) noexcept
	{
		D3D12_RESOURCE_DESC result{};
		result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		result.Width = a_size;
		result.Height = 1;
		result.DepthOrArraySize = 1;
		result.MipLevels = 1;
		result.SampleDesc.Count = 1;
		result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		return result;
	}

	[[nodiscard]] bool CreateDescriptorHeap(
		ID3D12Device*                  a_device,
		D3D12_DESCRIPTOR_HEAP_TYPE     a_type,
		D3D12_DESCRIPTOR_HEAP_FLAGS    a_flags,
		ComPtr<ID3D12DescriptorHeap>& a_result, UINT a_count) noexcept
	{
		D3D12_DESCRIPTOR_HEAP_DESC description{};
		description.Type = a_type;
		description.NumDescriptors = a_count;
		description.Flags = a_flags;
		return CheckResult(
			a_device->CreateDescriptorHeap(
				&description,
				IID_PPV_ARGS(a_result.GetAddressOf())),
			"descriptor-heap creation");
	}

	// Adapted from Dear ImGui 1.90.8-docking imgui_impl_dx12.cpp at
	// 6d948ab47ecf984239af01434f3ed03808dbf188 (MIT). Each uploaded image gets
	// its own heap; the renderer retains resources until GPU completion.
	[[nodiscard]] bool Upload(
		ID3D12Device*              a_device,
		ID3D12GraphicsCommandList* a_commandList,
		const unsigned char*       a_pixels,
		int                        a_width,
		int                        a_height,
		Texture&                   a_result) noexcept
	{
		a_result.Reset();
		if (!a_device || !a_commandList ||
			a_commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !a_pixels ||
			a_width <= 0 || a_height <= 0 ||
			a_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			a_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
			logger::error("Rejected invalid RGBA texture {}x{}", a_width, a_height);
			return false;
		}

		const auto rowBytes = static_cast<std::uint64_t>(a_width) * 4;
		const auto uploadPitch =
			(rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
			~static_cast<std::uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		const auto uploadSize = uploadPitch * static_cast<std::uint64_t>(a_height);
		Texture candidate;
		if (!CreateDescriptorHeap(
				a_device,
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
				candidate.ShaderHeap) ||
			candidate.ShaderHeap->GetGPUDescriptorHandleForHeapStart().ptr == 0 ||
			!CreateDescriptorHeap(a_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				D3D12_DESCRIPTOR_HEAP_FLAG_NONE, candidate.ViewHeap)) {
			return false;
		}

		D3D12_RESOURCE_DESC texture{};
		texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texture.Width = static_cast<UINT64>(a_width);
		texture.Height = static_cast<UINT>(a_height);
		texture.DepthOrArraySize = 1;
		texture.MipLevels = 1;
		texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		texture.SampleDesc.Count = 1;
		texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
		if (!CheckResult(
				a_device->CreateCommittedResource(
					&defaultHeap,
					D3D12_HEAP_FLAG_NONE,
					&texture,
					D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr,
					IID_PPV_ARGS(candidate.TextureResource.GetAddressOf())),
				"texture creation")) {
			return false;
		}

		const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
		const auto upload = BufferDescription(uploadSize);
		if (!CheckResult(
				a_device->CreateCommittedResource(
					&uploadHeap,
					D3D12_HEAP_FLAG_NONE,
					&upload,
					D3D12_RESOURCE_STATE_GENERIC_READ,
					nullptr,
					IID_PPV_ARGS(candidate.UploadBuffer.GetAddressOf())),
				"texture-upload creation")) {
			return false;
		}

		void* mapped{};
		constexpr D3D12_RANGE noCpuReads{ 0, 0 };
		if (!CheckResult(candidate.UploadBuffer->Map(0, &noCpuReads, &mapped), "texture-upload map") ||
			!mapped) {
			return false;
		}
		for (int row = 0; row < a_height; ++row) {
			std::memcpy(
				static_cast<std::byte*>(mapped) +
					static_cast<std::size_t>(row) * static_cast<std::size_t>(uploadPitch),
				a_pixels + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes),
				static_cast<std::size_t>(rowBytes));
		}
		const D3D12_RANGE cpuWrites{ 0, static_cast<SIZE_T>(uploadSize) };
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
		destination.pResource = candidate.TextureResource.Get();
		destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		a_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = candidate.TextureResource.Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		a_commandList->ResourceBarrier(1, &barrier);

		D3D12_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		view.Texture2D.MipLevels = 1;
		a_device->CreateShaderResourceView(
			candidate.TextureResource.Get(),
			&view,
			candidate.ViewHeap->GetCPUDescriptorHandleForHeapStart());
		a_device->CopyDescriptorsSimple(1,
			candidate.ShaderHeap->GetCPUDescriptorHandleForHeapStart(),
			candidate.ViewHeap->GetCPUDescriptorHandleForHeapStart(),
			D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		a_result = std::move(candidate);
		return true;
	}
}
