#include "rendering/StreamlineUIPrototype.h"

#include "rendering/D3D12Renderer.h"
#include "rendering/CommandListState.h"
#include "rendering/OverlayTrace.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <wrl/client.h>

namespace SFSEMenuFramework::StreamlineUIPrototype
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		struct StructType final
		{
			std::uint32_t Data1;
			std::uint16_t Data2;
			std::uint16_t Data3;
			std::uint8_t Data4[8];
		};

		struct BaseStructure final
		{
			void* Next;
			StructType Type;
			std::size_t Version;
		};

		struct Extent final
		{
			std::uint32_t Top;
			std::uint32_t Left;
			std::uint32_t Width;
			std::uint32_t Height;
		};

		struct Resource final
		{
			BaseStructure Base;
			std::uint8_t Type;
			std::uint8_t Padding[7];
			void* Native;
			void* Memory;
			void* View;
			std::uint32_t State;
			std::uint32_t Width;
			std::uint32_t Height;
			std::uint32_t NativeFormat;
			std::uint32_t MipLevels;
			std::uint32_t ArrayLayers;
			std::uint64_t GpuVirtualAddress;
			std::uint32_t Flags;
			std::uint32_t Usage;
			std::uint16_t InternalFlags;
			std::uint16_t Reserved;
		};

		struct ResourceTag final
		{
			BaseStructure Base;
			Resource* ResourceData;
			std::uint32_t Type;
			std::uint32_t Lifecycle;
			Extent Region;
		};

		static_assert(offsetof(Resource, Native) == 40);
		static_assert(offsetof(Resource, State) == 64);
		static_assert(sizeof(Resource) == 112);
		static_assert(offsetof(ResourceTag, ResourceData) == 32);
		static_assert(offsetof(ResourceTag, Type) == 40);
		static_assert(sizeof(ResourceTag) == 64);

		using Result = std::int32_t;
		using SetTagFn = Result (*)(const void*, const ResourceTag*, std::uint32_t, void*);
		using SerializeRootFn = HRESULT(WINAPI*)(
			const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

		constexpr std::uint32_t uiColorAndAlpha = 23;
		constexpr std::size_t descriptorCount = 64;
		constexpr std::uint64_t uiRouteFreshMilliseconds = 250;
		constexpr StructType resourceTagType{
			0x4C6A5AAD, 0xB445, 0x496C,
			{ 0x87, 0xFF, 0x1A, 0xF3, 0x84, 0x5B, 0xE6, 0x53 }
		};
		constexpr GUID streamlineNativeInterface{
			0xADEC44E2,
			0x61F0,
			0x45C3,
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

		struct State final
		{
			ComPtr<ID3D12Device> Device;
			ComPtr<ID3D12DescriptorHeap> SrvHeap;
			ComPtr<ID3D12DescriptorHeap> RtvHeap;
			ComPtr<ID3D12RootSignature> RootSignature;
			ComPtr<ID3DBlob> VS;
			ComPtr<ID3DBlob> PS;
			ComPtr<ID3D12PipelineState> Pipeline;
			ComPtr<ID3D12Resource> Overlay;
			std::vector<ComPtr<ID3D12Resource>> RetiredOverlays;
			std::vector<ComPtr<ID3D12DescriptorHeap>> RetiredSrvHeaps;
			std::vector<ComPtr<ID3D12PipelineState>> RetiredPipelines;
			UINT RtvStride{};
			std::uint64_t Width{};
			std::uint32_t Height{};
			DXGI_FORMAT PipelineFormat{ DXGI_FORMAT_UNKNOWN };
			std::size_t NextTargetDescriptor{};
			bool Initialized{};
			bool Failed{};
		};


		std::atomic<SetTagFn> originalSetTag{};
		std::atomic_flag installed{};
		std::atomic_flag rejectionLogged{};
		std::atomic_flag heapWaitLogged{};
		std::atomic_flag rendererLogged{};
		std::atomic_flag routeLogged{};
		std::atomic<bool> uiPathActive{ false };
		std::atomic<std::uint64_t> lastUiRenderTick{};

		thread_local ID3D12GraphicsCommandList* lastRenderedCommandList{};
		thread_local std::uint64_t lastRenderedEpoch{};

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


		void LogRejection(const char* a_reason) noexcept
		{
			if (!rejectionLogged.test_and_set(std::memory_order_relaxed)) {
				logger::warn("Streamline UI overlay rejected: {}", a_reason);
			}
		}

		[[nodiscard]] bool SameType(const StructType& a_left, const StructType& a_right) noexcept
		{
			return std::memcmp(&a_left, &a_right, sizeof(StructType)) == 0;
		}

		[[nodiscard]] bool SameIdentity(IUnknown* a_left, IUnknown* a_right) noexcept
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

		[[nodiscard]] bool ResolveNativeDevice(
			ID3D12Device* a_device,
			ComPtr<ID3D12Device>& a_native) noexcept
		{
			a_native.Reset();
			if (!a_device) {
				return false;
			}

			ID3D12Device* native{};
			if (SUCCEEDED(a_device->QueryInterface(
					streamlineNativeInterface,
					reinterpret_cast<void**>(&native))) &&
				native) {
				a_native.Attach(native);
				return true;
			}

			a_native = a_device;
			return true;
		}

		[[nodiscard]] bool ResolveNativeCommandList(
			ID3D12GraphicsCommandList* a_commandList,
			ComPtr<ID3D12GraphicsCommandList>& a_native) noexcept
		{
			a_native.Reset();
			if (!a_commandList) {
				return false;
			}

			ID3D12GraphicsCommandList* native{};
			if (SUCCEEDED(a_commandList->QueryInterface(
					streamlineNativeInterface,
					reinterpret_cast<void**>(&native))) &&
				native) {
				a_native.Attach(native);
				return true;
			}

			a_native = a_commandList;
			return true;
		}


		void STDMETHODCALLTYPE SetHeaps(
			ID3D12GraphicsCommandList* a_list,
			UINT a_count,
			ID3D12DescriptorHeap* const* a_heaps)
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
					"Streamline UI overlay shader {} failed: {}", a_target,
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
			heap.NumDescriptors = static_cast<UINT>(descriptorCount);
			if (FAILED(a_state.Device->CreateDescriptorHeap(
					&heap, IID_PPV_ARGS(a_state.RtvHeap.GetAddressOf())))) {
				return false;
			}
			a_state.RtvStride =
				a_state.Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			return true;
		}

		[[nodiscard]] bool EnsureOverlay(
			State& a_state,
			std::uint64_t a_width,
			std::uint32_t a_height)
		{
			if (a_state.Overlay && a_state.Width == a_width && a_state.Height == a_height) {
				return true;
			}
			if (a_state.Overlay) {
				D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
				heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				heapDesc.NumDescriptors = 1;
				heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
				ComPtr<ID3D12DescriptorHeap> nextHeap;
				if (FAILED(a_state.Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&nextHeap)))) {
					return false;
				}
				// Old command lists still reference the old GPU descriptor after a resize.
				a_state.RetiredSrvHeaps.push_back(a_state.SrvHeap);
				a_state.SrvHeap = std::move(nextHeap);
				a_state.RetiredOverlays.push_back(a_state.Overlay);
				a_state.Overlay.Reset();
			}

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
					&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					nullptr, IID_PPV_ARGS(a_state.Overlay.GetAddressOf())))) {
				return false;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			a_state.Device->CreateShaderResourceView(
				a_state.Overlay.Get(), &srv,
				a_state.SrvHeap->GetCPUDescriptorHandleForHeapStart());

			D3D12_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			a_state.Device->CreateRenderTargetView(
				a_state.Overlay.Get(), &rtv,
				a_state.RtvHeap->GetCPUDescriptorHandleForHeapStart());

			a_state.Width = a_width;
			a_state.Height = a_height;
			return true;
		}

		[[nodiscard]] bool EnsurePipeline(State& a_state, DXGI_FORMAT a_format)
		{
			if (a_state.Pipeline && a_state.PipelineFormat == a_format) {
				return true;
			}
			if (a_state.Pipeline) {
				a_state.RetiredPipelines.push_back(a_state.Pipeline);
				a_state.Pipeline.Reset();
			}

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

		[[nodiscard]] bool EnsureRenderer(
			State& a_state,
			ID3D12Device* a_device,
			std::uint64_t a_width,
			std::uint32_t a_height,
			DXGI_FORMAT a_format)
		{
			if (a_state.Failed) {
				return false;
			}

			if (!a_state.Initialized) {
				a_state.Device = a_device;
				if (!D3D12Renderer::Initialize(a_device) || !CreateStaticResources(a_state)) {
					a_state.Failed = true;
					return false;
				}
				a_state.Initialized = true;
				if (!rendererLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Streamline UI overlay renderer initialized");
				}
			} else if (!SameIdentity(a_state.Device.Get(), a_device)) {
				LogRejection("UI resource device changed");
				return false;
			}

			return EnsureOverlay(a_state, a_width, a_height) && EnsurePipeline(a_state, a_format);
		}

		[[nodiscard]] bool GetTargetRtv(
			State& a_state,
			ID3D12Resource* a_target,
			DXGI_FORMAT a_format,
			D3D12_CPU_DESCRIPTOR_HANDLE& a_handle) noexcept
		{
			if (!a_state.RtvHeap || !a_state.Device || !a_target || descriptorCount < 2) {
				return false;
			}

			const auto slot = 1 + (a_state.NextTargetDescriptor++ % (descriptorCount - 1));
			a_handle = a_state.RtvHeap->GetCPUDescriptorHandleForHeapStart();
			a_handle.ptr += slot * a_state.RtvStride;

			D3D12_RENDER_TARGET_VIEW_DESC rtv{};
			rtv.Format = a_format;
			rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			a_state.Device->CreateRenderTargetView(a_target, &rtv, a_handle);
			return true;
		}

		void Transition(
			ID3D12GraphicsCommandList* a_list,
			ID3D12Resource* a_resource,
			D3D12_RESOURCE_STATES a_before,
			D3D12_RESOURCE_STATES a_after) noexcept
		{
			if (!a_list || !a_resource || a_before == a_after) {
				return;
			}
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			a_list->ResourceBarrier(1, &barrier);
		}

		[[nodiscard]] bool RenderFramework(
			const ResourceTag& a_tag,
			void* a_commandBuffer) noexcept
		{
			std::scoped_lock routingLock{ CommandListState::RoutingMutex() };
			if (!a_tag.ResourceData || !a_tag.ResourceData->Native) {
				return false;
			}
			if (!a_commandBuffer) {
				LogRejection("UI tag has no command buffer");
				return false;
			}
			if (a_tag.ResourceData->State == UINT_MAX) {
				LogRejection("UI resource state is unknown");
				return false;
			}

			ComPtr<ID3D12Resource> target;
			if (FAILED(reinterpret_cast<IUnknown*>(a_tag.ResourceData->Native)->QueryInterface(
					IID_PPV_ARGS(target.GetAddressOf()))) ||
				!target) {
				LogRejection("UI native resource is not ID3D12Resource");
				return false;
			}

			const auto desc = target->GetDesc();
			const auto targetFormat = RtvFormat(desc.Format);
			if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
				desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
				desc.SampleDesc.Count != 1 || targetFormat == DXGI_FORMAT_UNKNOWN ||
				desc.Width < 256 || desc.Height < 256 ||
				(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
				LogRejection("UI texture description is unsupported");
				return false;
			}

			ComPtr<ID3D12GraphicsCommandList> proxyList;
			if (FAILED(reinterpret_cast<IUnknown*>(a_commandBuffer)->QueryInterface(
					IID_PPV_ARGS(proxyList.GetAddressOf()))) ||
				!proxyList || proxyList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
				LogRejection("command buffer is not a direct D3D12 command list");
				return false;
			}

			ComPtr<ID3D12GraphicsCommandList> nativeList;
			if (!ResolveNativeCommandList(proxyList.Get(), nativeList) || !nativeList) {
				LogRejection("could not resolve native D3D12 command list");
				return false;
			}

			ComPtr<ID3D12Device> targetDevice;
			ComPtr<ID3D12Device> listDevice;
			ComPtr<ID3D12Device> nativeListDevice;
			if (FAILED(target->GetDevice(IID_PPV_ARGS(targetDevice.GetAddressOf()))) ||
				FAILED(proxyList->GetDevice(IID_PPV_ARGS(listDevice.GetAddressOf()))) ||
				!targetDevice || !listDevice ||
				!ResolveNativeDevice(listDevice.Get(), nativeListDevice) ||
				!SameIdentity(targetDevice.Get(), nativeListDevice.Get())) {
				LogRejection("command list and UI resource device identity differ");
				return false;
			}

			if (!CommandListState::Install(nativeList.Get())) {
				LogRejection("could not install complete command-list state tracking");
				return false;
			}
			CommandListState::Snapshot savedState;
			const char* stateReason = "none";
			if (!CommandListState::Capture(nativeList.Get(), savedState, &stateReason)) {
				OverlayTrace::StateRejected(stateReason);
				if (!heapWaitLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Streamline UI overlay waiting for complete command-list state");
				}
				return false;
			}
			D3D12Renderer::DescriptorHeapSnapshot heapSnapshot;
			heapSnapshot.Count = savedState.HeapCount;
			for (UINT i = 0; i < savedState.HeapCount; ++i) {
				heapSnapshot.Heaps[i] = savedState.Heaps[i].Get();
			}
			const auto epoch = savedState.Epoch;
			if (lastRenderedCommandList == proxyList.Get() && lastRenderedEpoch == epoch) {
				OverlayTrace::Record(OverlayTrace::Duplicate);
				return true;
			}

			std::scoped_lock lock{ GetMutex() };
			auto& state = GetState();
			if (!EnsureRenderer(state, targetDevice.Get(), desc.Width, desc.Height, targetFormat)) {
				return false;
			}

			CommandListState::InjectionScope injection;
			struct RestoreOnExit {
				ID3D12GraphicsCommandList* List;
				const CommandListState::Snapshot& Saved;
				~RestoreOnExit() { Saved.Restore(List); }
			} restore{ nativeList.Get(), savedState };
			const auto overlayRtv = state.RtvHeap->GetCPUDescriptorHandleForHeapStart();
			Transition(
				nativeList.Get(), state.Overlay.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			constexpr float clear[4]{};
			nativeList->ClearRenderTargetView(overlayRtv, clear, 0, nullptr);
			const bool recorded = D3D12Renderer::Render(
				nativeList.Get(), state.Overlay.Get(), heapSnapshot, &SetHeaps);
			OverlayTrace::Record(recorded ? OverlayTrace::ImGuiDraw : OverlayTrace::ImGuiSkipped);
			Transition(
				nativeList.Get(), state.Overlay.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			D3D12_CPU_DESCRIPTOR_HANDLE targetRtv{};
			if (!GetTargetRtv(state, target.Get(), targetFormat, targetRtv)) {
				return false;
			}

			const auto originalState =
				static_cast<D3D12_RESOURCE_STATES>(a_tag.ResourceData->State);
			Transition(
				nativeList.Get(), target.Get(), originalState,
				D3D12_RESOURCE_STATE_RENDER_TARGET);

			ID3D12DescriptorHeap* heaps[]{ state.SrvHeap.Get() };
			nativeList->SetDescriptorHeaps(1, heaps);
			nativeList->SetGraphicsRootSignature(state.RootSignature.Get());
			nativeList->SetPipelineState(state.Pipeline.Get());
			nativeList->SetGraphicsRootDescriptorTable(
				0, state.SrvHeap->GetGPUDescriptorHandleForHeapStart());

			const D3D12_VIEWPORT viewport{
				0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height),
				0.0f, 1.0f
			};
			const D3D12_RECT scissor{
				0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)
			};
			nativeList->RSSetViewports(1, &viewport);
			nativeList->RSSetScissorRects(1, &scissor);
			nativeList->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
			nativeList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			nativeList->DrawInstanced(3, 1, 0, 0);

			Transition(
				nativeList.Get(), target.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, originalState);
			SetHeaps(
				nativeList.Get(), heapSnapshot.Count, heapSnapshot.Heaps.data());

			lastRenderedCommandList = proxyList.Get();
			lastRenderedEpoch = epoch;
			lastUiRenderTick.store(::GetTickCount64(), std::memory_order_release);
			if (!uiPathActive.exchange(true, std::memory_order_acq_rel)) {
				OverlayTrace::Record(OverlayTrace::RouteOn);
			}
			if (!routeLogged.test_and_set(std::memory_order_relaxed)) {
				logger::info("Streamline UI overlay rendering SFSE-MF into UIColorAndAlpha with state restoration");
			}
			return true;
		}

		Result SetTagThunk(
			const void* a_viewport,
			const ResourceTag* a_tags,
			std::uint32_t a_count,
			void* a_commandBuffer) noexcept
		{
			if (a_tags && a_count <= 64) {
				for (std::uint32_t index = 0; index < a_count; ++index) {
					const auto& tag = a_tags[index];
					if (!SameType(tag.Base.Type, resourceTagType) ||
						tag.Type != uiColorAndAlpha) {
						continue;
					}

					if (!tag.ResourceData || !tag.ResourceData->Native) {
						OverlayTrace::Record(OverlayTrace::NullTag);
						if (uiPathActive.exchange(false, std::memory_order_acq_rel)) {
							OverlayTrace::Record(OverlayTrace::RouteOff);
						}
					} else {
						OverlayTrace::Record(OverlayTrace::Tag);
						if (!RenderFramework(tag, a_commandBuffer)) {
							OverlayTrace::Record(OverlayTrace::Rejected);
							if (uiPathActive.exchange(false, std::memory_order_acq_rel)) {
								OverlayTrace::Record(OverlayTrace::RouteOff);
							}
						}
					}
					break;
				}
			}

			OverlayTrace::Report();
			const auto original = originalSetTag.load(std::memory_order_acquire);
			return original ? original(a_viewport, a_tags, a_count, a_commandBuffer) : -1;
		}

		[[nodiscard]] bool PatchImport(void** a_original) noexcept
		{
			auto* base = reinterpret_cast<std::byte*>(::GetModuleHandleW(nullptr));
			if (!base) {
				return false;
			}
			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return false;
			}
			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) {
				return false;
			}
			const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (!directory.VirtualAddress || !directory.Size) {
				return false;
			}

			auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
			for (; descriptor->Name; ++descriptor) {
				const auto* dll = reinterpret_cast<const char*>(base + descriptor->Name);
				if (_stricmp(dll, "sl.interposer.dll") != 0 || !descriptor->OriginalFirstThunk) {
					continue;
				}
				auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
				auto* entries = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
				for (; names->u1.AddressOfData; ++names, ++entries) {
					if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
						continue;
					}
					auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
					if (std::strcmp(reinterpret_cast<const char*>(import->Name), "slSetTag") != 0) {
						continue;
					}

					*a_original = reinterpret_cast<void*>(entries->u1.Function);
					DWORD oldProtect{};
					if (!::VirtualProtect(
							reinterpret_cast<void*>(&entries->u1.Function),
							sizeof(entries->u1.Function), PAGE_READWRITE, &oldProtect)) {
						*a_original = nullptr;
						return false;
					}
					entries->u1.Function = reinterpret_cast<ULONG_PTR>(&SetTagThunk);
					DWORD ignored{};
					static_cast<void>(::VirtualProtect(
						reinterpret_cast<void*>(&entries->u1.Function),
						sizeof(entries->u1.Function), oldProtect, &ignored));
					return true;
				}
			}
			return false;
		}
	}

	bool Install() noexcept
	{
		if (installed.test_and_set(std::memory_order_acq_rel)) {
			return originalSetTag.load(std::memory_order_acquire) != nullptr;
		}

		void* original{};
		if (!PatchImport(&original) || !original) {
			logger::warn("Streamline UI overlay could not hook slSetTag");
			return false;
		}
		originalSetTag.store(reinterpret_cast<SetTagFn>(original), std::memory_order_release);
		logger::info("Streamline UI overlay installed");
		return true;
	}

	bool HasRecentUIRender() noexcept
	{
		if (!uiPathActive.load(std::memory_order_acquire)) {
			return false;
		}
		const auto last = lastUiRenderTick.load(std::memory_order_acquire);
		if (!last || ::GetTickCount64() - last > uiRouteFreshMilliseconds) {
			OverlayTrace::Record(OverlayTrace::Timeout);
			uiPathActive.store(false, std::memory_order_release);
			return false;
		}
		return true;
	}
}
