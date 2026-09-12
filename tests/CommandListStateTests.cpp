#include "rendering/CommandListState.h"
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// RenderHooks.cpp also contains the unused game-pass installer.
namespace SFSEMenuFramework::RenderHooks::Detail
{
	void ResetRegion() noexcept {}
	bool EnsureCommandListHooks() noexcept { return false; }
	void ActivateRegionAfterScaleformEnd() noexcept {}
	void FinalizeRegionBeforeComposite() noexcept {}
}

namespace
{
	using Microsoft::WRL::ComPtr;
	using namespace SFSEMenuFramework::CommandListState;
	void Check(bool ok, const char* message)
	{
		if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
	}
	void Hr(HRESULT result) { Check(SUCCEEDED(result), "D3D12 call"); }

	ComPtr<ID3D12RootSignature> MakeRoot(ID3D12Device* device)
	{
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.Num32BitValues = 8;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable = { 1, &range };
		D3D12_ROOT_SIGNATURE_DESC desc{};
		desc.NumParameters = 2;
		desc.pParameters = params;
		ComPtr<ID3DBlob> blob;
		Hr(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr));
		ComPtr<ID3D12RootSignature> root;
		Hr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
		return root;
	}

	ComPtr<ID3D12PipelineState> MakePipeline(ID3D12Device* device, ID3D12RootSignature* root)
	{
		constexpr char shader[] = "[numthreads(1,1,1)] void main() {}";
		ComPtr<ID3DBlob> blob;
		Hr(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &blob, nullptr));
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = root;
		desc.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
		ComPtr<ID3D12PipelineState> pipeline;
		Hr(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
		return pipeline;
	}
}

int main()
{
	ComPtr<IDXGIFactory4> factory;
	Hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
	ComPtr<IDXGIAdapter> adapter;
	Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
	ComPtr<ID3D12Device> device;
	Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
	auto root = MakeRoot(device.Get());
	auto pipeline = MakePipeline(device.Get(), root.Get());
	ComPtr<ID3D12CommandAllocator> allocator;
	Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
	ComPtr<ID3D12GraphicsCommandList> list;
	Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
	Check(Install(list.Get()), "install state hooks on WARP");
	Snapshot before;
	Check(!Capture(list.Get(), before), "reject a list whose beginning was not observed");
	Hr(list->Close());
	Hr(list->Reset(allocator.Get(), pipeline.Get()));

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 2;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ComPtr<ID3D12DescriptorHeap> heap;
	Hr(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)));
	ID3D12DescriptorHeap* heaps[]{ heap.Get() };
	list->SetDescriptorHeaps(1, heaps);
	list->SetGraphicsRootSignature(root.Get());
	list->SetComputeRootSignature(root.Get());
	const UINT constants[]{ 17, 29, 43 };
	list->SetGraphicsRoot32BitConstants(0, 3, constants, 2);
	list->SetGraphicsRoot32BitConstant(0, 99, 3);
	const auto table = heap->GetGPUDescriptorHandleForHeapStart();
	list->SetGraphicsRootDescriptorTable(1, table);
	list->SetComputeRootDescriptorTable(1, table);
	const D3D12_VIEWPORT viewport{ 2, 3, 640, 480, 0, 1 };
	const D3D12_RECT scissor{ 5, 7, 630, 470 };
	const FLOAT blend[]{ 0.2f, 0.3f, 0.4f, 0.5f };
	list->RSSetViewports(1, &viewport);
	list->RSSetScissorRects(1, &scissor);
	list->OMSetBlendFactor(blend);
	list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
	list->IASetIndexBuffer(nullptr);
	list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
	Check(Capture(list.Get(), before), "capture complete state");
	Check(before.Graphics[0].Constants[3] == 99 && before.Graphics[0].Constants[4] == 43,
		"track partial constant updates");
	Check(before.Graphics[1].Value == table.ptr && before.ComputeTableSet[1], "track both root tables");
	Check(before.Pipeline.Get() == pipeline.Get(), "capture initial Reset pipeline");
	Check(before.Topology == D3D_PRIMITIVE_TOPOLOGY_LINELIST, "capture topology slot");

	// Record unrelated state, then exercise real D3D12 setters during replay.
	list->SetGraphicsRootSignature(nullptr);
	list->SetDescriptorHeaps(0, nullptr);
	before.Restore(list.Get());
	Snapshot after;
	Check(Capture(list.Get(), after), "capture restored state");
	Check(after.GraphicsRoot == before.GraphicsRoot && after.Heaps[0] == before.Heaps[0], "restore root and heaps");
	Check(after.Graphics[0].Constants == before.Graphics[0].Constants &&
		after.Graphics[0].Written == before.Graphics[0].Written, "restore sparse constants");
	Check(after.Graphics[1].Value == before.Graphics[1].Value && after.ComputeTableSet[1], "restore descriptor tables");
	Check(std::memcmp(&after.Viewports[0], &viewport, sizeof(viewport)) == 0 &&
		std::memcmp(&after.Scissors[0], &scissor, sizeof(scissor)) == 0, "restore raster state");
	Check(after.Blend == before.Blend && after.Topology == before.Topology, "restore blend and topology");
	{
		InjectionScope injection;
		list->SetGraphicsRoot32BitConstant(0, 123, 3);
		Check(Capture(list.Get(), after) && after.Graphics[0].Constants[3] == 99,
			"framework calls cannot contaminate game state");
		before.Restore(list.Get());
	}
	list->ClearState(pipeline.Get());
	Check(!Capture(list.Get(), after), "ClearState invalidates root bindings");
	Hr(list->Close());
	Hr(list->Reset(allocator.Get(), nullptr));
	Check(!Capture(list.Get(), after), "Reset requires newly observed bindings");
	Hr(list->Close());
	std::puts("PASS: WARP command-list capture, replay, partial roots, injection isolation, reset guards");
}
