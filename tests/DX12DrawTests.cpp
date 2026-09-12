#include "DX12Draw.h"
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>

namespace
{
    using Microsoft::WRL::ComPtr;
    void Check(bool ok) { if (!ok) { std::abort(); } }
    using CreateResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
        D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
        const D3D12_CLEAR_VALUE*, REFIID, void**);
    CreateResource original{};
    int failAt{}, uploads{};
    HRESULT STDMETHODCALLTYPE FailUpload(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heap,
        D3D12_HEAP_FLAGS flags, const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
        const D3D12_CLEAR_VALUE* clear, REFIID iid, void** result)
    {
        if (heap->Type == D3D12_HEAP_TYPE_UPLOAD && ++uploads == failAt) {
            *result = nullptr;
            return E_OUTOFMEMORY;
        }
        return original(device, heap, flags, desc, state, clear, iid, result);
    }
    void Replace(void** slot, void* value)
    {
        DWORD protection{};
        Check(VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection) != 0);
        *slot = value;
        DWORD ignored{};
        Check(VirtualProtect(slot, sizeof(void*), protection, &ignored) != 0);
    }
}

int main()
{
    ComPtr<IDXGIFactory4> factory;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> adapter;
    Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))));
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))));
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(SUCCEEDED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap))));
    auto** slot = *reinterpret_cast<void***>(device.Get()) + 27; // ID3D12Device::CreateCommittedResource
    original = reinterpret_cast<CreateResource>(*slot);
    for (int failure = 1; failure <= 2; ++failure) {
        ImGui::CreateContext();
        Check(ImGui_ImplDX12_Init(device.Get(), 1, DXGI_FORMAT_R8G8B8A8_UNORM, heap.Get(),
            heap->GetCPUDescriptorHandleForHeapStart(), heap->GetGPUDescriptorHandleForHeapStart()));
        Check(ImGui_ImplDX12_CreateDeviceObjects());
        ImDrawData data;
        data.OwnerViewport = ImGui::GetMainViewport();
        data.DisplaySize = {64, 64};
        failAt = failure;
        uploads = 0;
        Replace(slot, reinterpret_cast<void*>(&FailUpload));
        const bool recorded = ImGui_ImplDX12_RenderDrawDataChecked(&data, list.Get(), {}, true);
        Replace(slot, reinterpret_cast<void*>(original));
        Check(!recorded && uploads == failure);
        // Failure did not poison the next use of the same backend buffer slot.
        Check(ImGui_ImplDX12_RenderDrawDataChecked(&data, list.Get(), {}, false));
        ImGui_ImplDX12_Shutdown();
        ImGui::DestroyContext();
    }
    Check(SUCCEEDED(list->Close()));
    std::puts("PASS: vertex/index allocation failures return false and the next draw recovers");
}
