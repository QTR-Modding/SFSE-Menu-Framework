#pragma once

#include <d3d12.h>
#include <array>
#include <wrl/client.h>

namespace SFSEMenuFramework
{
	// One completion marker per use, not merely the last recorded frame. GPU
	// submissions may finish in a different order from CPU recording.
	class GpuResourceRetirement
	{
	public:
		struct Use
		{
			Microsoft::WRL::ComPtr<ID3D12Resource> Marker;
			std::array<Microsoft::WRL::ComPtr<IUnknown>, 3> Resources;
			UINT Value{};
			bool Pending{};
		};
		[[nodiscard]] Use* Begin(ID3D12Device* device,
			IUnknown* texture, IUnknown* heap, IUnknown* pipeline);
		static void End(Use& use, ID3D12GraphicsCommandList2* list);
		std::size_t Collect();
	private:
		// Fixed capacity keeps Use pointers stable and applies backpressure at 64 uses.
		std::array<Use, 64> uses;
	};
}
