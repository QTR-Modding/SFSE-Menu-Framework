#include "rendering/GpuResourceRetirement.h"
#include "rendering/D3D12Texture.h"

namespace SFSEMenuFramework
{
	namespace
	{
		bool Read(ID3D12Resource* marker, UINT& value)
		{
			void* data{};
			const D3D12_RANGE read{ 0, sizeof(UINT) };
			if (FAILED(marker->Map(0, &read, &data)) || !data) { return false; }
			value = *static_cast<const volatile UINT*>(data);
			const D3D12_RANGE noWrites{};
			marker->Unmap(0, &noWrites);
			return true;
		}
	}

	std::size_t GpuResourceRetirement::Collect()
	{
		std::size_t pending{};
		for (auto& use : uses) {
			if (!use.Pending) { continue; }
			UINT value{};
			if (Read(use.Marker.Get(), value) && value == use.Value) {
				use.Resources = {};
				use.Pending = false;
			} else {
				++pending;
			}
		}
		return pending;
	}

	GpuResourceRetirement::Use* GpuResourceRetirement::Begin(
		ID3D12Device* device, IUnknown* texture, IUnknown* heap, IUnknown* pipeline)
	{
		Collect();
		Use* available{};
		for (auto& use : uses) {
			if (!use.Pending) { available = &use; break; }
		}
		if (!available) { return nullptr; }
		if (!available->Marker) {
			const auto hp = D3D12Textures::HeapProperties(D3D12_HEAP_TYPE_READBACK);
			const auto desc = D3D12Textures::BufferDescription(sizeof(UINT));
			if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&available->Marker)))) { return nullptr; }
		}
		UINT previous{};
		if (!Read(available->Marker.Get(), previous)) { return nullptr; }
		available->Value = previous + 1;
		available->Resources = { texture, heap, pipeline };
		available->Pending = true;
		return available;
	}

	void GpuResourceRetirement::End(Use& use, ID3D12GraphicsCommandList2* list)
	{
		const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker{ use.Marker->GetGPUVirtualAddress(), use.Value };
		constexpr auto mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
		list->WriteBufferImmediate(1, &marker, &mode);
	}
}
