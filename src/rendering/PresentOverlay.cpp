#include "rendering/PresentOverlay.h"

#include "platform/win32/Win32PlatformInternal.h"
#include "rendering/D3D12Renderer.h"
#include "rendering/StreamlineUIPrototype.h"
#include "rendering/CommandListState.h"

#include <RE/C/CreationRenderer.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <wrl/client.h>

namespace SFSEMenuFramework::PresentOverlay
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
			IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
		using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
		using SerializeRootFn = HRESULT(WINAPI*)(
			const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

		constexpr std::size_t presentSlot = 8;
		constexpr std::size_t present1Slot = 22;
		constexpr GUID streamlineNative{
			0xADEC44E2, 0x61F0, 0x45C3,
			{ 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF }
		};

		constexpr char vertexShader[] = R"(
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O main(uint id : SV_VertexID) {
	O o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.p = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
	return o;
})";
		constexpr char pixelShader[] = R"(
Texture2D t : register(t0);
SamplerState s : register(s0);
float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return t.Sample(s, uv);
})";

		enum class HookState : std::uint8_t
		{
			Uninitialized,
			Installing,
			Ready
		};

		struct Frame final
		{
			ComPtr<ID3D12CommandAllocator> Allocator;
			std::uint64_t FenceValue{};
		};

		struct State final
		{
			ComPtr<ID3D12Device> Device;
			ComPtr<ID3D12CommandQueue> Queue;
			ComPtr<ID3D12GraphicsCommandList> List;
			ComPtr<ID3D12Fence> Fence;
			std::vector<Frame> Frames;
			std::uint64_t NextFence{ 1 };

			ComPtr<ID3D12RootSignature> RootSignature;
			ComPtr<ID3DBlob> VS;
			ComPtr<ID3DBlob> PS;
			ComPtr<ID3D12DescriptorHeap> SrvHeap;
			ComPtr<ID3D12DescriptorHeap> RtvHeap;
			UINT RtvStride{};

			ComPtr<ID3D12Resource> Overlay;
			std::uint64_t Width{};
			std::uint32_t Height{};
			ComPtr<ID3D12PipelineState> Pipeline;
			DXGI_FORMAT PipelineFormat{ DXGI_FORMAT_UNKNOWN };
			bool Initialized{};
			bool SubmissionFailed{};
		};

		std::atomic<HookState> hookState{ HookState::Uninitialized };
		std::atomic<PresentFn> originalPresent{};
		std::atomic<Present1Fn> originalPresent1{};
		std::atomic_flag deviceChangeLogged{};
		std::atomic_flag queueChangeLogged{};
		std::atomic_flag signalFailureLogged{};
		thread_local std::uint32_t presentDepth{};

		[[nodiscard]] State& GetState()
		{
			static auto* state = new State();
			return *state;
		}

		[[nodiscard]] std::mutex& GetMutex()
		{
			static auto* mutex = new std::mutex();
			return *mutex;
		}

		[[nodiscard]] bool IsStarfieldSwapChain(IDXGISwapChain3* a_swapChain) noexcept
		{
			HWND window{};
			if (!a_swapChain || FAILED(a_swapChain->GetHwnd(&window)) || !window) {
				return false;
			}

			auto& platform = Win32Platform::Detail::Shared();
			return platform.SubclassActive.load(std::memory_order_acquire) &&
			       !platform.HostWindowTearingDown.load(std::memory_order_acquire) &&
			       platform.InitializedHostWindow.load(std::memory_order_acquire) == window;
		}

		[[nodiscard]] bool HasSameIdentity(IUnknown* a_left, IUnknown* a_right) noexcept
		{
			if (!a_left || !a_right) {
				return false;
			}

			ComPtr<IUnknown> left;
			ComPtr<IUnknown> right;
			return SUCCEEDED(a_left->QueryInterface(IID_PPV_ARGS(left.GetAddressOf()))) &&
			       SUCCEEDED(a_right->QueryInterface(IID_PPV_ARGS(right.GetAddressOf()))) &&
			       left.Get() == right.Get();
		}

		template <class T>
		[[nodiscard]] bool GetNative(T* a_object, ComPtr<T>& a_result)
		{
			if (!a_object) {
				return false;
			}
			T* native{};
			if (SUCCEEDED(a_object->QueryInterface(streamlineNative, reinterpret_cast<void**>(&native))) &&
				native) {
				a_result.Attach(native);
				return true;
			}
			return SUCCEEDED(a_object->QueryInterface(IID_PPV_ARGS(a_result.GetAddressOf())));
		}

		void STDMETHODCALLTYPE SetHeaps(
			ID3D12GraphicsCommandList* a_list, UINT a_count, ID3D12DescriptorHeap* const* a_heaps)
		{
			a_list->SetDescriptorHeaps(a_count, a_heaps);
		}

		[[nodiscard]] DXGI_FORMAT RtvFormat(DXGI_FORMAT a_format) noexcept
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return DXGI_FORMAT_R8G8B8A8_UNORM;
			case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				return DXGI_FORMAT_B8G8R8A8_UNORM;
			case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				return DXGI_FORMAT_R10G10B10A2_UNORM;
			case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				return DXGI_FORMAT_R16G16B16A16_FLOAT;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		[[nodiscard]] ComPtr<ID3DBlob> Compile(const char* a_source, const char* a_target)
		{
			ComPtr<ID3DBlob> code;
			ComPtr<ID3DBlob> errors;
			if (FAILED(::D3DCompile(
					a_source, std::strlen(a_source), nullptr, nullptr, nullptr, "main", a_target,
					D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf()))) {
				logger::critical(
					"Present overlay shader {} failed: {}", a_target,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				return {};
			}
			return code;
		}

		[[nodiscard]] SerializeRootFn RootSerializer()
		{
			static const auto serializer = []() -> SerializeRootFn {
				const auto module =
					::LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
				return module ? reinterpret_cast<SerializeRootFn>(
								::GetProcAddress(module, "D3D12SerializeRootSignature")) :
								nullptr;
			}();
			return serializer;
		}

		[[nodiscard]] bool CreateStaticResources(State& a_state)
		{
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 1;

			D3D12_ROOT_PARAMETER parameter{};
			parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameter.DescriptorTable.NumDescriptorRanges = 1;
			parameter.DescriptorTable.pDescriptorRanges = &range;
			parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rootDesc{};
			rootDesc.NumParameters = 1;
			rootDesc.pParameters = &parameter;
			rootDesc.NumStaticSamplers = 1;
			rootDesc.pStaticSamplers = &sampler;

			const auto serialize = RootSerializer();
			ComPtr<ID3DBlob> rootBlob;
			ComPtr<ID3DBlob> errors;
			if (!serialize ||
				FAILED(serialize(
					&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.GetAddressOf(),
					errors.GetAddressOf())) ||
				!rootBlob ||
				FAILED(a_state.Device->CreateRootSignature(
					0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
					IID_PPV_ARGS(a_state.RootSignature.GetAddressOf())))) {
				return false;
			}

			a_state.VS = Compile(vertexShader, "vs_5_0");
			a_state.PS = Compile(pixelShader, "ps_5_0");
			if (!a_state.VS || !a_state.PS) {
				return false;
			}

			D3D12_DESCRIPTOR_HEAP_DESC heap{};
			heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heap.NumDescriptors = 1;
			heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(a_state.Device->CreateDescriptorHeap(
					&heap, IID_PPV_ARGS(a_state.SrvHeap.GetAddressOf())))) {
				return false;
			}

			heap = {};
			heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			heap.NumDescriptors = 2;
			if (FAILED(a_state.Device->CreateDescriptorHeap(
					&heap, IID_PPV_ARGS(a_state.RtvHeap.GetAddressOf())))) {
				return false;
			}
			a_state.RtvStride =
				a_state.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

			return SUCCEEDED(a_state.Device->CreateFence(
				0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(a_state.Fence.GetAddressOf())));
		}

		[[nodiscard]] bool IsComplete(State& a_state, std::uint64_t a_value) noexcept
		{
			return !a_value || a_state.Fence->GetCompletedValue() >= a_value;
		}

		[[nodiscard]] bool AreAllComplete(State& a_state) noexcept
		{
			for (const auto& frame : a_state.Frames) {
				if (!IsComplete(a_state, frame.FenceValue)) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool EnsureFrames(State& a_state, UINT a_count)
		{
			if (!a_count) {
				return false;
			}
			const auto oldSize = a_state.Frames.size();
			if (oldSize < a_count) {
				a_state.Frames.resize(a_count);
				for (std::size_t i = oldSize; i < a_state.Frames.size(); ++i) {
					if (FAILED(a_state.Device->CreateCommandAllocator(
							D3D12_COMMAND_LIST_TYPE_DIRECT,
							IID_PPV_ARGS(a_state.Frames[i].Allocator.GetAddressOf())))) {
						return false;
					}
				}
			}
			if (!a_state.List) {
				if (FAILED(a_state.Device->CreateCommandList(
						0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_state.Frames[0].Allocator.Get(),
						nullptr, IID_PPV_ARGS(a_state.List.GetAddressOf()))) ||
					FAILED(a_state.List->Close())) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool EnsureOverlay(State& a_state, std::uint64_t a_width, std::uint32_t a_height)
		{
			if (a_state.Overlay && a_state.Width == a_width && a_state.Height == a_height) {
				return true;
			}
			if (!AreAllComplete(a_state)) {
				return false;
			}
			a_state.Overlay.Reset();

			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			heap.CreationNodeMask = heap.VisibleNodeMask = 1;

			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = a_width;
			desc.Height = a_height;
			desc.DepthOrArraySize = desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			if (FAILED(a_state.Device->CreateCommittedResource(
					&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					nullptr, IID_PPV_ARGS(a_state.Overlay.GetAddressOf())))) {
				return false;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			a_state.Device->CreateShaderResourceView(
				a_state.Overlay.Get(), &srv, a_state.SrvHeap->GetCPUDescriptorHandleForHeapStart());

			D3D12_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			a_state.Device->CreateRenderTargetView(
				a_state.Overlay.Get(), &rtv, a_state.RtvHeap->GetCPUDescriptorHandleForHeapStart());

			a_state.Width = a_width;
			a_state.Height = a_height;
			return true;
		}

		[[nodiscard]] bool EnsurePipeline(State& a_state, DXGI_FORMAT a_format)
		{
			if (a_state.Pipeline && a_state.PipelineFormat == a_format) {
				return true;
			}
			if (!AreAllComplete(a_state)) {
				return false;
			}
			a_state.Pipeline.Reset();

			D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature = a_state.RootSignature.Get();
			desc.VS = { a_state.VS->GetBufferPointer(), a_state.VS->GetBufferSize() };
			desc.PS = { a_state.PS->GetBufferPointer(), a_state.PS->GetBufferSize() };
			desc.SampleMask = UINT_MAX;
			desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			desc.RasterizerState.DepthClipEnable = TRUE;
			desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			desc.NumRenderTargets = 1;
			desc.RTVFormats[0] = a_format;
			desc.SampleDesc.Count = 1;

			auto& blend = desc.BlendState.RenderTarget[0];
			blend.BlendEnable = TRUE;
			blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

			if (FAILED(a_state.Device->CreateGraphicsPipelineState(
					&desc, IID_PPV_ARGS(a_state.Pipeline.GetAddressOf())))) {
				return false;
			}
			a_state.PipelineFormat = a_format;
			return true;
		}

		[[nodiscard]] bool Initialize(State& a_state, ID3D12Device* a_device, UINT a_bufferCount)
		{
			if (a_state.SubmissionFailed) {
				return false;
			}

			auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
			ComPtr<ID3D12CommandQueue> queue;
			if (!renderer || !renderer->GetGraphicsQueue() ||
				!GetNative(
					reinterpret_cast<ID3D12CommandQueue*>(renderer->GetGraphicsQueue()), queue)) {
				return false;
			}

			if (a_state.Initialized) {
				if (!D3D12Renderer::HasSameDeviceIdentity(a_state.Device.Get(), a_device)) {
					if (!deviceChangeLogged.test_and_set(std::memory_order_relaxed)) {
						logger::critical(
							"DXGI Present overlay device changed; rendering is disabled for the replacement device");
					}
					return false;
				}
				if (!HasSameIdentity(a_state.Queue.Get(), queue.Get())) {
					if (!queueChangeLogged.test_and_set(std::memory_order_relaxed)) {
						logger::critical(
							"DXGI Present overlay graphics queue changed; rendering is disabled for the replacement queue");
					}
					return false;
				}
				return EnsureFrames(a_state, a_bufferCount);
			}

			a_state.Device = a_device;
			a_state.Queue = std::move(queue);

			ComPtr<ID3D12Device> queueDevice;
			if (FAILED(a_state.Queue->GetDevice(IID_PPV_ARGS(queueDevice.GetAddressOf()))) ||
				!D3D12Renderer::HasSameDeviceIdentity(a_device, queueDevice.Get()) ||
				!D3D12Renderer::Initialize(a_device) ||
				!CreateStaticResources(a_state) ||
				!EnsureFrames(a_state, a_bufferCount)) {
				return false;
			}

			a_state.Initialized = true;
			logger::info("DXGI Present overlay renderer initialized");
			return true;
		}

		void Draw(IDXGISwapChain* a_swapChain) noexcept
		{
			std::scoped_lock routingLock{ CommandListState::RoutingMutex() };
			if (StreamlineUIPrototype::HasRecentUIRender()) {
				return;
			}

			std::scoped_lock lock{ GetMutex() };
			CommandListState::InjectionScope injection;

			ComPtr<IDXGISwapChain3> swapChain;
			if (!a_swapChain ||
				FAILED(a_swapChain->QueryInterface(IID_PPV_ARGS(swapChain.GetAddressOf()))) ||
				!IsStarfieldSwapChain(swapChain.Get())) {
				return;
			}

			DXGI_SWAP_CHAIN_DESC1 swapDesc{};
			if (FAILED(swapChain->GetDesc1(&swapDesc)) || !swapDesc.BufferCount) {
				return;
			}

			const UINT index = swapChain->GetCurrentBackBufferIndex();
			if (index >= swapDesc.BufferCount) {
				return;
			}

			ComPtr<ID3D12Resource> backBuffer;
			if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(backBuffer.GetAddressOf())))) {
				return;
			}
			const auto backDesc = backBuffer->GetDesc();
			const auto finalFormat = RtvFormat(backDesc.Format);
			if (backDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
				backDesc.SampleDesc.Count != 1 || backDesc.Width < 256 || backDesc.Height < 256 ||
				finalFormat == DXGI_FORMAT_UNKNOWN) {
				return;
			}

			ComPtr<ID3D12Device> device;
			if (FAILED(backBuffer->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))) {
				return;
			}

			auto& state = GetState();
			if (!Initialize(state, device.Get(), swapDesc.BufferCount) ||
				!EnsureOverlay(state, backDesc.Width, backDesc.Height) ||
				!EnsurePipeline(state, finalFormat)) {
				return;
			}

			auto& frame = state.Frames[index];
			if (!IsComplete(state, frame.FenceValue) || FAILED(frame.Allocator->Reset()) ||
				FAILED(state.List->Reset(frame.Allocator.Get(), nullptr))) {
				return;
			}

			D3D12_RESOURCE_BARRIER overlay{};
			overlay.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			overlay.Transition.pResource = state.Overlay.Get();
			overlay.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			overlay.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			overlay.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			state.List->ResourceBarrier(1, &overlay);

			const auto overlayRtv = state.RtvHeap->GetCPUDescriptorHandleForHeapStart();
			constexpr float clear[4]{};
			state.List->ClearRenderTargetView(overlayRtv, clear, 0, nullptr);

			D3D12Renderer::DescriptorHeapSnapshot restore{};
			restore.Count = 1;
			restore.Heaps[0] = state.SrvHeap.Get();
			D3D12Renderer::Render(state.List.Get(), state.Overlay.Get(), restore, &SetHeaps);

			std::swap(overlay.Transition.StateBefore, overlay.Transition.StateAfter);
			state.List->ResourceBarrier(1, &overlay);

			D3D12_RESOURCE_BARRIER back{};
			back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			back.Transition.pResource = backBuffer.Get();
			back.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			back.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			back.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			state.List->ResourceBarrier(1, &back);

			auto finalRtv = overlayRtv;
			finalRtv.ptr += state.RtvStride;
			D3D12_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = finalFormat;
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			state.Device->CreateRenderTargetView(backBuffer.Get(), &rtv, finalRtv);

			ID3D12DescriptorHeap* heaps[]{ state.SrvHeap.Get() };
			state.List->SetDescriptorHeaps(1, heaps);
			state.List->SetGraphicsRootSignature(state.RootSignature.Get());
			state.List->SetPipelineState(state.Pipeline.Get());
			state.List->SetGraphicsRootDescriptorTable(
				0, state.SrvHeap->GetGPUDescriptorHandleForHeapStart());

			const D3D12_VIEWPORT viewport{
				0.0f, 0.0f, static_cast<float>(backDesc.Width), static_cast<float>(backDesc.Height),
				0.0f, 1.0f
			};
			const D3D12_RECT scissor{
				0, 0, static_cast<LONG>(backDesc.Width), static_cast<LONG>(backDesc.Height)
			};
			state.List->RSSetViewports(1, &viewport);
			state.List->RSSetScissorRects(1, &scissor);
			state.List->OMSetRenderTargets(1, &finalRtv, FALSE, nullptr);
			state.List->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			state.List->DrawInstanced(3, 1, 0, 0);

			std::swap(back.Transition.StateBefore, back.Transition.StateAfter);
			state.List->ResourceBarrier(1, &back);
			if (FAILED(state.List->Close())) {
				return;
			}

			ID3D12CommandList* lists[]{ state.List.Get() };
			state.Queue->ExecuteCommandLists(1, lists);
			const auto fenceValue = state.NextFence++;
			if (FAILED(state.Queue->Signal(state.Fence.Get(), fenceValue))) {
				state.SubmissionFailed = true;
				if (!signalFailureLogged.test_and_set(std::memory_order_relaxed)) {
					logger::critical(
						"DXGI Present overlay fence signal failed; rendering is disabled");
				}
				return;
			}
			frame.FenceValue = fenceValue;
		}

		HRESULT STDMETHODCALLTYPE PresentThunk(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags) noexcept
		{
			const auto original = originalPresent.load(std::memory_order_acquire);
			if (!original) {
				return E_FAIL;
			}
			if (presentDepth++ == 0) {
				Draw(a_swapChain);
			}
			const auto result = original(a_swapChain, a_sync, a_flags);
			--presentDepth;
			return result;
		}

		HRESULT STDMETHODCALLTYPE Present1Thunk(
			IDXGISwapChain1* a_swapChain,
			UINT a_sync,
			UINT a_flags,
			const DXGI_PRESENT_PARAMETERS* a_parameters) noexcept
		{
			const auto original = originalPresent1.load(std::memory_order_acquire);
			if (!original) {
				return E_FAIL;
			}
			if (presentDepth++ == 0) {
				Draw(a_swapChain);
			}
			const auto result = original(a_swapChain, a_sync, a_flags, a_parameters);
			--presentDepth;
			return result;
		}

		[[nodiscard]] bool WriteSlot(void** a_vtable, std::size_t a_slot, void* a_value)
		{
			DWORD oldProtect{};
			if (!::VirtualProtect(&a_vtable[a_slot], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				return false;
			}
			a_vtable[a_slot] = a_value;
			DWORD ignored{};
			static_cast<void>(::VirtualProtect(
				&a_vtable[a_slot], sizeof(void*), oldProtect, &ignored));
			return a_vtable[a_slot] == a_value;
		}

		[[nodiscard]] bool Install()
		{
			auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
			ComPtr<ID3D12CommandQueue> queue;
			if (!renderer || !renderer->GetGraphicsQueue() ||
				!GetNative(
					reinterpret_cast<ID3D12CommandQueue*>(renderer->GetGraphicsQueue()), queue)) {
				return false;
			}

			const auto dxgi = ::LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			const auto createFactory = dxgi ?
				reinterpret_cast<CreateFactoryFn>(::GetProcAddress(dxgi, "CreateDXGIFactory2")) :
				nullptr;
			if (!createFactory) {
				return false;
			}

			ComPtr<IDXGIFactory2> factory;
			if (FAILED(createFactory(0, IID_PPV_ARGS(factory.GetAddressOf())))) {
				return false;
			}

			const HWND window = ::CreateWindowExW(
				WS_EX_TOOLWINDOW, L"STATIC", L"SFSEMenuFrameworkPresent", WS_OVERLAPPED,
				0, 0, 2, 2, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
			if (!window) {
				return false;
			}

			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.Width = desc.Height = 2;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = 2;
			desc.Scaling = DXGI_SCALING_STRETCH;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

			ComPtr<IDXGISwapChain1> dummy;
			const auto result = factory->CreateSwapChainForHwnd(
				queue.Get(), window, &desc, nullptr, nullptr, dummy.GetAddressOf());
			::DestroyWindow(window);
			if (FAILED(result) || !dummy) {
				return false;
			}

			auto** vtable = *reinterpret_cast<void***>(dummy.Get());
			auto* present = vtable ? vtable[presentSlot] : nullptr;
			auto* present1 = vtable ? vtable[present1Slot] : nullptr;
			if (!present || !present1 ||
				present == reinterpret_cast<void*>(&PresentThunk) ||
				present1 == reinterpret_cast<void*>(&Present1Thunk)) {
				return false;
			}

			originalPresent.store(reinterpret_cast<PresentFn>(present), std::memory_order_release);
			originalPresent1.store(reinterpret_cast<Present1Fn>(present1), std::memory_order_release);
			if (!WriteSlot(vtable, presentSlot, reinterpret_cast<void*>(&PresentThunk))) {
				return false;
			}
			if (!WriteSlot(vtable, present1Slot, reinterpret_cast<void*>(&Present1Thunk))) {
				static_cast<void>(WriteSlot(vtable, presentSlot, present));
				return false;
			}

			logger::info("DXGI Present overlay installed");
			return true;
		}
	}

	bool EnsureInstalled() noexcept
	{
		const auto current = hookState.load(std::memory_order_acquire);
		if (current != HookState::Uninitialized) {
			return current == HookState::Ready;
		}

		HookState expected = HookState::Uninitialized;
		if (!hookState.compare_exchange_strong(
				expected, HookState::Installing, std::memory_order_acq_rel)) {
			return expected == HookState::Ready;
		}

		const bool installed = Install();
		hookState.store(
			installed ? HookState::Ready : HookState::Uninitialized,
			std::memory_order_release);
		return installed;
	}
}
