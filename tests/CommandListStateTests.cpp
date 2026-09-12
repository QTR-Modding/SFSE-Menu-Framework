#include "rendering/CommandListState.h"
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

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

	void TestConcurrentRecordings(ID3D12Device* device, const Snapshot& seed)
	{
		constexpr std::size_t workerCount = 8;
		constexpr std::size_t rounds = 32;
		std::array<std::uint64_t, workerCount * rounds * 2> epochs{};
		std::barrier ready{ static_cast<std::ptrdiff_t>(workerCount) };
		std::array<std::jthread, workerCount> workers;
		for (std::size_t worker = 0; worker < workerCount; ++worker) {
			workers[worker] = std::jthread([&, worker] {
				ComPtr<ID3D12CommandAllocator> allocator;
				Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
				ComPtr<ID3D12GraphicsCommandList> list;
				Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
					allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
				Hr(list->Close());
				Snapshot captured;
				for (std::size_t round = 0; round < rounds; ++round) {
					ready.arrive_and_wait();
					Hr(allocator->Reset());
					Hr(list->Reset(allocator.Get(), seed.Pipeline.Get()));
					list->SetComputeRootSignature(seed.ComputeRoot.Get());
					seed.Restore(list.Get());
					const UINT marker = static_cast<UINT>((worker + 1) * 1000 + round);
					list->SetGraphicsRoot32BitConstant(0, marker, 3);
					Check(Capture(list.Get(), captured) && captured.Graphics[0].Constants[3] == marker,
						"concurrent command lists keep their own root arguments");
					const auto index = (worker * rounds + round) * 2;
					epochs[index] = captured.Epoch;
					{
						InjectionScope injection;
						list->SetGraphicsRoot32BitConstant(0, marker + 1, 3);
						Check(Capture(list.Get(), captured) && captured.Graphics[0].Constants[3] == marker,
							"concurrent injection scopes do not contaminate tracked state");
					}
					list->ClearState(seed.Pipeline.Get());
					Check(!Capture(list.Get(), captured), "concurrent ClearState invalidates bindings");
					list->SetComputeRootSignature(seed.ComputeRoot.Get());
					seed.Restore(list.Get());
					Check(Capture(list.Get(), captured) && captured.Epoch > epochs[index],
						"concurrent ClearState starts a fresh recording");
					epochs[index + 1] = captured.Epoch;
					Hr(list->Close());
					Check(!Capture(list.Get(), captured), "concurrent Close retires the recording");
				}
			});
		}
		for (auto& worker : workers) { worker.join(); }
		std::sort(epochs.begin(), epochs.end());
		Check(epochs.front() != 0 && std::adjacent_find(epochs.begin(), epochs.end()) == epochs.end(),
			"recording IDs remain globally unique across threads");
	}

	void TestOverlayRecordings(ID3D12Device* device, const Snapshot& seed)
	{
		std::array<ComPtr<ID3D12CommandAllocator>, 2> allocators;
		std::array<ComPtr<ID3D12GraphicsCommandList>, 2> lists;
		std::array<std::uint64_t, 2> epochs{};
		std::array<ComPtr<ID3D12Resource>, 2> targets;
		D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
		D3D12_RESOURCE_DESC texture{};
		texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texture.Width = texture.Height = 4;
		texture.DepthOrArraySize = texture.MipLevels = 1;
		texture.SampleDesc.Count = 1;
		texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		for (std::size_t i = 0; i < lists.size(); ++i) {
			Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&targets[i])));
			Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
			Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				allocators[i].Get(), nullptr, IID_PPV_ARGS(&lists[i])));
			Hr(lists[i]->Close());
			Hr(lists[i]->Reset(allocators[i].Get(), seed.Pipeline.Get()));
			lists[i]->SetComputeRootSignature(seed.ComputeRoot.Get());
			seed.Restore(lists[i].Get());
			Snapshot captured;
			Check(Capture(lists[i].Get(), captured), "capture overlay recording");
			epochs[i] = captured.Epoch;
		}
		auto* first = lists[0].Get();
		auto* second = lists[1].Get();
		auto* a = targets[0].Get();
		auto* b = targets[1].Get();
		RecordOverlay(first, epochs[0], a, 0);
		RecordOverlay(second, epochs[1], a, 7);
		RecordOverlay(first, epochs[0], b, 11);
		Check(FindOverlay(first, epochs[0], a) == 0 && FindOverlay(first, epochs[0], b) == 11,
			"A/B/A target tags retain both entries, including generation zero");
		Check(FindOverlay(second, epochs[1], a) == 7,
			"interleaved command lists retain independent overlay generations");
		std::jthread handoff([&] {
			Check(FindOverlay(first, epochs[0], a) == 0, "overlay history survives recording thread handoff");
			RecordOverlay(first, epochs[0], b, 12);
		});
		handoff.join();
		Check(FindOverlay(first, epochs[0], b) == 12, "another thread can update the same recording");
		RecordOverlay(first, epochs[1], a, 99);
		Check(!FindOverlay(first, epochs[1], a) && FindOverlay(first, epochs[0], a) == 0,
			"wrong-epoch overlay access cannot change a live recording");
		first->ClearState(seed.Pipeline.Get());
		Check(!FindOverlay(first, epochs[0], a), "ClearState retires overlay history");
		first->SetComputeRootSignature(seed.ComputeRoot.Get());
		seed.Restore(first);
		Snapshot captured;
		Check(Capture(first, captured), "capture new ClearState epoch");
		RecordOverlay(first, epochs[0], a, 99);
		Check(!FindOverlay(first, captured.Epoch, a), "stale writes cannot seed the next recording");
		RecordOverlay(first, captured.Epoch, a, 13);
		Hr(first->Close());
		RecordOverlay(first, captured.Epoch, a, 99);
		Check(!FindOverlay(first, captured.Epoch, a), "Close rejects overlay reads and writes");
		Hr(first->Reset(allocators[0].Get(), seed.Pipeline.Get()));
		first->SetComputeRootSignature(seed.ComputeRoot.Get());
		seed.Restore(first);
		Check(Capture(first, captured) && !FindOverlay(first, captured.Epoch, a),
			"Reset begins without overlays from an earlier recording");
		Hr(first->Close());
		Check(FindOverlay(second, epochs[1], a) == 7, "closing another list preserves this recording");
		Hr(second->Close());
		Check(!FindOverlay(second, epochs[1], a), "final Close retires the remaining overlay history");
	}
}

void TestTargetDescriptors(ID3D12Device*, ID3D12RootSignature*, ID3D12DescriptorHeap*);

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
	{
		auto invalid = before;
		invalid.Heaps[0].Reset();
		Check(!invalid.Ready(), "reject a null entry in a nonempty heap snapshot");
	}
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
	// Keep every COM object alive: allocator address reuse must not hide the old 128-entry bug.
	std::vector<ComPtr<ID3D12GraphicsCommandList>> recordings;
	std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
	auto lastEpoch = before.Epoch;
	for (int i = 0; i < 256; ++i) {
		ComPtr<ID3D12CommandAllocator> nextAllocator;
		Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nextAllocator)));
		ComPtr<ID3D12GraphicsCommandList> next;
		Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			nextAllocator.Get(), nullptr, IID_PPV_ARGS(&next)));
		Hr(next->Close());
		Hr(next->Reset(nextAllocator.Get(), pipeline.Get()));
		before.Restore(next.Get());
		Check(Capture(next.Get(), after), "capture more than 128 simultaneous recordings");
		Check(after.Epoch > lastEpoch, "recording IDs are globally unique");
		lastEpoch = after.Epoch;
		recordings.push_back(std::move(next));
		allocators.push_back(std::move(nextAllocator));
	}
	for (std::size_t i = 0; i < recordings.size(); ++i) {
		auto* next = recordings[i].Get();
		Hr(next->Close());
		Check(!Capture(next, after), "Close retires a complete snapshot");
		Hr(next->Reset(allocators[i].Get(), pipeline.Get()));
		before.Restore(next);
		Check(Capture(next, after) && after.Epoch > lastEpoch, "reused list gets a fresh recording ID");
		lastEpoch = after.Epoch;
		Hr(next->Close());
		Check(!Capture(next, after), "reused recording is retired");
	}
	TestConcurrentRecordings(device.Get(), before);
	TestOverlayRecordings(device.Get(), before);
	TestTargetDescriptors(device.Get(), root.Get(), heap.Get());
	std::puts("PASS: WARP state replay, injection isolation, reset guards, 256 recordings and concurrent lifetimes");
}
