#include "rendering/CommandListState.h"
#include "rendering/GpuResourceRetirement.h"
#include "rendering/OverlayCompositor.h"
#include "rendering/D3D12Texture.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	using Microsoft::WRL::ComPtr;
	void Require(bool ok)
	{
		if (!ok) { std::fputs("FAIL: GPU target descriptor restoration\n", stderr); std::exit(1); }
	}
	void Hr(HRESULT hr) { Require(SUCCEEDED(hr)); }
	void TestExtents()
	{
		using SFSEMenuFramework::OverlayCompositor::Extent;
		D3D12_RECT rect{};
		Require(Extent{}.Resolve(1920, 1080, rect) && rect.right == 1920 && rect.bottom == 1080);
		Require(Extent{ 20, 10, 1900, 1000 }.Resolve(1920, 1080, rect) &&
			rect.left == 10 && rect.top == 20 && rect.right == 1910 && rect.bottom == 1020);
		Require(Extent{ 0, 0, 1920, 1080 }.Resolve(1920, 1080, rect));
		for (const auto invalid : { Extent{ 0, 0, 0, 1 }, Extent{ 0, 0, 1, 0 },
			Extent{ 1, 0, 0, 0 }, Extent{ 0, 1920, 1, 1 }, Extent{ 1080, 0, 1, 1 },
			Extent{ 0, 1, 1920, 1080 }, Extent{ 1, 0, 1920, 1080 },
			Extent{ 0, 1, UINT_MAX, 1 }, Extent{ UINT_MAX, UINT_MAX, 2, 2 } }) {
			Require(!invalid.Resolve(1920, 1080, rect));
		}
		Require(!Extent{}.Resolve(UINT64_MAX, 1080, rect));
		Require(!Extent{}.Resolve(1920, UINT_MAX, rect));
		Require(!Extent{}.Resolve(0, 1080, rect));
		Require(!Extent{}.Resolve(1920, 0, rect));
	}
	ComPtr<ID3DBlob> Compile(const char* source, const char* target)
	{
		ComPtr<ID3DBlob> code;
		Hr(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
			"main", target, 0, 0, &code, nullptr));
		return code;
	}
}

void TestTargetDescriptors(ID3D12Device* device, ID3D12RootSignature* root, ID3D12DescriptorHeap* shaderHeap)
{
	using namespace SFSEMenuFramework::CommandListState;
	TestExtents();
	const auto vs = Compile(
		"float4 main(uint i:SV_VertexID):SV_Position {"
		"float2 p=float2((i<<1)&2,i&2); return float4(p*float2(2,-2)+float2(-1,1),0,1);}", "vs_5_0");
	const auto ps = Compile(
		"struct O {float4 a:SV_Target0;float4 b:SV_Target1;float d:SV_Depth;};"
		"O main(){O o;o.a=float4(0,1,0,1);o.b=float4(0,0,1,1);o.d=0.25;return o;}", "ps_5_0");
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
	pd.pRootSignature = root;
	pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	pd.SampleMask = UINT_MAX;
	pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pd.DepthStencilState.DepthEnable = TRUE;
	pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pd.NumRenderTargets = 2;
	pd.RTVFormats[0] = pd.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	pd.SampleDesc.Count = 1;
	pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	ComPtr<ID3D12PipelineState> pipeline;
	Hr(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)));
	D3D12_COMMAND_QUEUE_DESC qd{};
	ComPtr<ID3D12CommandQueue> queue;
	Hr(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
	ComPtr<ID3D12Fence> fence;
	Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	Require(event != nullptr);
	SFSEMenuFramework::GpuResourceRetirement retirement;
	namespace Compositor = SFSEMenuFramework::OverlayCompositor;
	Compositor::Shaders shaders;
	ComPtr<ID3D12PipelineState> compositePipeline;
	Require(Compositor::CreateShaders(device, shaders));
	Require(Compositor::CreatePipeline(device, shaders, DXGI_FORMAT_R8G8B8A8_UNORM, compositePipeline));
	for (UINT pass = 0; pass < 3; ++pass) {
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
		Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
		Hr(list->Close());
		Hr(list->Reset(allocator.Get(), pipeline.Get()));
		std::array<ComPtr<ID3D12Resource>, 6> textures;
		std::array<ComPtr<ID3D12Resource>, 6> readbacks;
		std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 6> layouts{};
		ComPtr<ID3D12DescriptorHeap> rtv, dsv;
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 4;
		Hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv)));
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		hd.NumDescriptors = 2;
		Hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv)));
		std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6> handles{};
		for (UINT i = 0; i < 6; ++i) {
			const bool depth = i >= 4;
			D3D12_HEAP_PROPERTIES hp{};
			hp.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC td{};
			td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			td.Width = td.Height = 4;
			td.DepthOrArraySize = td.MipLevels = 1;
			td.SampleDesc.Count = 1;
			td.Format = depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
			td.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			Hr(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
				depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET,
				nullptr, IID_PPV_ARGS(&textures[i])));
			if (depth) {
				handles[i] = dsv->GetCPUDescriptorHandleForHeapStart();
				handles[i].ptr += (i - 4) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
				device->CreateDepthStencilView(textures[i].Get(), nullptr, handles[i]);
				list->ClearDepthStencilView(handles[i], D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
			} else {
				handles[i] = rtv->GetCPUDescriptorHandleForHeapStart();
				handles[i].ptr += i * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
				device->CreateRenderTargetView(textures[i].Get(), nullptr, handles[i]);
				constexpr float red[]{ 1, 0, 0, 1 };
				list->ClearRenderTargetView(handles[i], red, 0, nullptr);
			}
			UINT64 size{};
			device->GetCopyableFootprints(&td, 0, 1, 0, &layouts[i], nullptr, nullptr, &size);
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = size;
			buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			hp.Type = D3D12_HEAP_TYPE_READBACK;
			Hr(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &buffer,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbacks[i])));
		}
		const D3D12_VIEWPORT vp{ 0, 0, 4, 4, 0, 1 };
		const D3D12_RECT rect{ 0, 0, 4, 4 };
		list->RSSetViewports(1, &vp);
		list->RSSetScissorRects(1, &rect);
		list->SetGraphicsRootSignature(root);
		list->SetDescriptorHeaps(1, &shaderHeap);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->OMSetRenderTargets(2, handles.data(), pass == 0, &handles[4]);
		ComPtr<ID3D12Resource> predicate;
		if (pass == 2) {
			const auto heap = SFSEMenuFramework::D3D12Textures::HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = 16;
			buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&predicate)));
			void* data{};
			Hr(predicate->Map(0, nullptr, &data));
			const UINT64 values[]{ 1, 0 };
			std::memcpy(data, values, sizeof(values));
			predicate->Unmap(0, nullptr);
			// The zero predicate suppresses game draws. The overlay must still draw.
			list->SetPredication(predicate.Get(), 8, D3D12_PREDICATION_OP_EQUAL_ZERO);
		}
		Snapshot saved;
		Require(Capture(list.Get(), saved));
		// The original slots now identify different resources.
		device->CreateRenderTargetView(textures[2].Get(), nullptr, handles[0]);
		device->CreateRenderTargetView(textures[3].Get(), nullptr, handles[1]);
		device->CreateDepthStencilView(textures[5].Get(), nullptr, handles[4]);
		list->OMSetRenderTargets(2, handles.data(), FALSE, &handles[4]);
		ComPtr<ID3D12DescriptorHeap> overlaySrv, overlayRtv;
		hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		Hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&overlaySrv)));
		hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
		Hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&overlayRtv)));
		ComPtr<ID3D12Resource> overlay;
		const D3D12_RECT region = pass == 1 ? D3D12_RECT{ 1, 2, 3, 3 } : rect;
		Require(Compositor::CreateTexture(device, region.right - region.left,
			region.bottom - region.top, overlaySrv.Get(), overlay));
		{
			InjectionScope injecting;
			list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition = { overlay.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
			list->ResourceBarrier(1, &barrier);
			constexpr float halfBlue[]{ 0, 0, 0.5f, 0.5f };
			list->ClearRenderTargetView(SFSEMenuFramework::D3D12Textures::RenderTargetView(
				device, overlayRtv.Get(), overlay.Get(),
				DXGI_FORMAT_R8G8B8A8_UNORM), halfBlue, 0, nullptr);
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			list->ResourceBarrier(1, &barrier);
			const auto compositeRtv = SFSEMenuFramework::D3D12Textures::RenderTargetView(
				device, overlayRtv.Get(), textures[3].Get(),
				DXGI_FORMAT_R8G8B8A8_UNORM);
			Compositor::Draw(list.Get(), shaders, compositePipeline.Get(), overlaySrv.Get(), compositeRtv, region);
			// Reuse the compositor's CPU descriptor before submission; the recorded draw must not change.
			static_cast<void>(SFSEMenuFramework::D3D12Textures::RenderTargetView(
				device, overlayRtv.Get(), textures[2].Get(), DXGI_FORMAT_R8G8B8A8_UNORM));
			saved.Restore(list.Get());
			list->DrawInstanced(3, 1, 0, 0);
		}
		list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
		list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
		Snapshot empty;
		Require(Capture(list.Get(), empty) && empty.RenderTargetCount == 0 && !empty.HasDepthTarget);
		for (UINT i = 0; i < 6; ++i) {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = textures[i].Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = i >= 4 ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			list->ResourceBarrier(1, &barrier);
			D3D12_TEXTURE_COPY_LOCATION from{}, to{};
			from.pResource = textures[i].Get();
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			to.pResource = readbacks[i].Get();
			to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			to.PlacedFootprint = layouts[i];
			list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		}
		ComPtr<ID3D12GraphicsCommandList2> markerList;
		Hr(list.As(&markerList));
		std::array<SFSEMenuFramework::GpuResourceRetirement::Use*, 64> uses{};
		for (auto& use : uses) {
			use = retirement.Begin(device, textures[0].Get(), rtv.Get(), pipeline.Get());
			Require(use != nullptr);
			SFSEMenuFramework::GpuResourceRetirement::End(*use, markerList.Get());
		}
		Require(retirement.Collect() == 64);
		Require(retirement.Begin(device, textures[0].Get(), rtv.Get(), pipeline.Get()) == nullptr);
		for (auto* use : uses) { Require(use->Resources[0] != nullptr); }
		Hr(list->Close());
		ID3D12CommandList* lists[]{ list.Get() };
		queue->ExecuteCommandLists(1, lists);
		Hr(queue->Signal(fence.Get(), pass + 1));
		Hr(fence->SetEventOnCompletion(pass + 1, event));
		Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0);
		Require(retirement.Collect() == 0);
		for (auto* use : uses) { Require(use->Resources[0] == nullptr); }
		for (UINT i = 0; i < 6; ++i) {
			void* data{};
			Hr(readbacks[i]->Map(0, nullptr, &data));
			for (UINT y = 0; y < 4; ++y) {
				for (UINT x = 0; x < 4; ++x) {
					const auto* pixel = static_cast<const unsigned char*>(data) + layouts[i].Offset +
						y * layouts[i].Footprint.RowPitch + x * 4;
					if (i >= 4) {
						float depth{};
						std::memcpy(&depth, pixel, sizeof(depth));
						Require(depth == (i == 4 && pass != 2 ? 0.25f : 1.0f));
					} else if (i == 3 && x >= static_cast<UINT>(region.left) && x < static_cast<UINT>(region.right) &&
						y >= static_cast<UINT>(region.top) && y < static_cast<UINT>(region.bottom)) {
						Require(pixel[0] == 127 && pixel[1] == 0 && pixel[2] == 128 && pixel[3] == 255);
					} else {
						Require(pixel[0] == (i >= 2 || pass == 2 ? 255 : 0) &&
							pixel[1] == (i == 0 && pass != 2 ? 255 : 0) &&
							pixel[2] == (i == 1 && pass != 2 ? 255 : 0) && pixel[3] == 255);
					}
				}
			}
			const D3D12_RANGE noWrites{};
			readbacks[i]->Unmap(0, &noWrites);
		}
	}
	CloseHandle(event);
	std::puts("PASS: shared overlay compositor draws premultiplied alpha and restores game drawing state");
	std::puts("PASS: submitted GPU draw restores overwritten RTV/DSV descriptors (contiguous and individual)");
	std::puts("PASS: resources retained before submission, released after GPU completion, and slots reused");
	std::puts("PASS: cropped overlay preserves pixels outside its validated extent");
	std::puts("PASS: overlay renders with game predication disabled, then restores suppressed game drawing");
}
