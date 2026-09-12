#pragma once

#include <d3d12.h>
#include <array>
#include <bitset>
#include <cstdint>
#include <mutex>
#include <memory>
#include <wrl/client.h>

namespace SFSEMenuFramework::CommandListState
{
	// Only the state touched by our ImGui backend/compositor is replayed.
	// Compute descriptor tables are included because changing heaps invalidates them too.
	struct RootArgument
	{
		enum class Kind { Unset, Table, Constants, CBV, SRV, UAV } Type{};
		UINT64 Value{};
		std::array<UINT, 64> Constants{};
		std::bitset<64> Written;
	};

	struct Snapshot
	{
		struct TargetDescriptors
		{
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> RTV;
			Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> DSV;
		};
		Microsoft::WRL::ComPtr<ID3D12Device> Device;
		std::shared_ptr<TargetDescriptors> OwnedTargets;
		bool Complete{};
		const char* InvalidReason{ "invalid arguments" };
		std::uint64_t Epoch{};
		Microsoft::WRL::ComPtr<ID3D12PipelineState> Pipeline;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> GraphicsRoot;
		Microsoft::WRL::ComPtr<ID3D12RootSignature> ComputeRoot;
		std::array<RootArgument, 64> Graphics;
		std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 64> ComputeTables{};
		std::bitset<64> ComputeTableSet;
		std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, 2> Heaps;
		UINT HeapCount{};
		std::array<D3D12_VIEWPORT, 16> Viewports{};
		UINT ViewportCount{};
		std::array<D3D12_RECT, 16> Scissors{};
		UINT ScissorCount{};
		std::array<FLOAT, 4> Blend{ 1, 1, 1, 1 };
		D3D12_PRIMITIVE_TOPOLOGY Topology{ D3D_PRIMITIVE_TOPOLOGY_UNDEFINED };
		D3D12_VERTEX_BUFFER_VIEW VertexBuffer{}; // ImGui touches slot zero only.
		D3D12_INDEX_BUFFER_VIEW IndexBuffer{};
		bool HasIndexBuffer{};
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> RenderTargets{};
		UINT RenderTargetCount{};
		D3D12_CPU_DESCRIPTOR_HANDLE DepthTarget{};
		bool HasDepthTarget{};

		[[nodiscard]] bool Ready() const noexcept;
		void Restore(ID3D12GraphicsCommandList* a_list) const;
	};

	// Observe one native/wrapped implementation continuously, starting at Reset.
	[[nodiscard]] bool Install(ID3D12GraphicsCommandList* a_list);
	[[nodiscard]] bool Capture(ID3D12GraphicsCommandList* a_list, Snapshot& a_snapshot,
		const char** a_reason = nullptr);
	std::mutex& RoutingMutex();

	class InjectionScope
	{
	public:
		InjectionScope();
		~InjectionScope();
		InjectionScope(const InjectionScope&) = delete;
		InjectionScope& operator=(const InjectionScope&) = delete;
	private:
		bool previous;
	};
}
