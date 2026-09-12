#include "rendering/StreamlineDiagnostic.h"
#include "rendering/RenderHooksInternal.h"

#include "rendering/D3D12Renderer.h"

#include <RE/C/CreationRenderer.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <array>
#include <atomic>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include <wrl/client.h>

namespace SFSEMenuFramework::RenderHooks::StreamlineDiagnostic
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using ResourceBarrierFunction = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*,
			UINT,
			const D3D12_RESOURCE_BARRIER*);
		using ResetFunction = HRESULT(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*,
			ID3D12CommandAllocator*,
			ID3D12PipelineState*);
		using ClearStateFunction = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*,
			ID3D12PipelineState*);
		using SetDescriptorHeapsFunction = D3D12Renderer::SetDescriptorHeapsFunction;

		constexpr std::array<std::size_t, 4> commandListSlots{ 10, 11, 26, 28 };
		constexpr GUID streamlineNativeInterface{
			0xADEC44E2,
			0x61F0,
			0x45C3,
			{ 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF }
		};

		constexpr const char* fullscreenVertexShader = R"(
struct VSOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID)
{
    VSOut output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}
)";

		constexpr const char* compositePixelShader = R"(
Texture2D overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return overlayTexture.Sample(overlaySampler, uv);
}
)";

		enum class HookState : std::uint8_t
		{
			Uninitialized,
			Installing,
			Ready,
			Failed
		};

		enum class PreviousRegion : std::uint8_t
		{
			Unknown,
			Normal,
			FrameGeneration
		};

		struct RegionState final
		{
			bool Active{ false };
			bool SawOrdinaryTarget{ false };
			bool SawCopyTarget{ false };
			bool FrameStarted{ false };
			std::uint8_t BarrierCallsAfterFirstCandidate{ 0 };
			PreviousRegion Previous{ PreviousRegion::Unknown };
		};

		struct DescriptorHeapState final
		{
			ComPtr<ID3D12GraphicsCommandList> CommandList;
			std::array<ComPtr<ID3D12DescriptorHeap>, 2> Heaps;
			UINT Count{ 0 };
		};

		struct CompositorState final
		{
			ComPtr<ID3D12Device> Device;
			ComPtr<ID3D12RootSignature> RootSignature;
			ComPtr<ID3D12PipelineState> Pipeline;
			ComPtr<ID3D12DescriptorHeap> ShaderHeap;
			ComPtr<ID3D12DescriptorHeap> RenderTargetHeap;
			ComPtr<ID3D12Resource> OverlayTarget;
			std::vector<ComPtr<ID3D12Resource>> RetiredTargets;
			UINT RenderTargetStride{ 0 };
			std::uint64_t Width{ 0 };
			std::uint32_t Height{ 0 };
			bool Failed{ false };
			bool ActiveLogged{ false };
		};

		std::atomic<HookState> state{ HookState::Uninitialized };
		std::atomic<PreviousRegion> previousRegion{ PreviousRegion::Unknown };
		std::atomic<ResetFunction> resetOriginal{ nullptr };
		std::atomic<ClearStateFunction> clearStateOriginal{ nullptr };
		std::atomic<ResourceBarrierFunction> resourceBarrierOriginal{ nullptr };
		std::atomic<SetDescriptorHeapsFunction> setDescriptorHeapsOriginal{ nullptr };
		std::atomic_bool compositorFallbackLogged{ false };

		thread_local RegionState regionState;
		thread_local DescriptorHeapState descriptorHeapState;
		thread_local bool internalD3D{ false };
		thread_local std::uint8_t selfTestSeen{};
		constexpr std::uint8_t resetSeen = 1U << 0;
		constexpr std::uint8_t clearStateSeen = 1U << 1;
		constexpr std::uint8_t resourceBarrierSeen = 1U << 2;
		constexpr std::uint8_t descriptorHeapsSeen = 1U << 3;
		constexpr std::uint8_t allSelfTestsSeen =
			resetSeen | clearStateSeen | resourceBarrierSeen | descriptorHeapsSeen;

		[[nodiscard]] CompositorState& GetCompositorState()
		{
			static auto* compositor = new CompositorState();
			return *compositor;
		}

		[[nodiscard]] std::mutex& GetCompositorMutex()
		{
			static auto* mutex = new std::mutex();
			return *mutex;
		}

		[[nodiscard]] bool IsStreamlineTarget(std::uintptr_t a_address) noexcept
		{
			if (!Detail::HasMemoryAccess(a_address, true)) {
				return false;
			}
			HMODULE module{};
			if (!::GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
						GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address),
					&module)) {
				return false;
			}
			wchar_t path[MAX_PATH]{};
			const auto length = ::GetModuleFileNameW(module, path, MAX_PATH);
			if (length == 0 || length >= MAX_PATH) {
				return false;
			}
			const auto* fileName = std::wcsrchr(path, L'\\');
			fileName = fileName ? fileName + 1 : path;
			return ::_wcsicmp(fileName, L"sl.interposer.dll") == 0;
		}

		[[nodiscard]] bool IsRgba8Format(DXGI_FORMAT a_format) noexcept
		{
			return a_format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
			       a_format == DXGI_FORMAT_R8G8B8A8_UNORM;
		}

		[[nodiscard]] bool IsCandidateResource(ID3D12Resource* a_resource) noexcept
		{
			if (!a_resource) {
				return false;
			}
			const auto description = a_resource->GetDesc();
			return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
			       D3D12Renderer::IsRenderTargetFormat(description.Format) &&
			       description.DepthOrArraySize == 1 && description.MipLevels == 1 &&
			       description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0 &&
			       description.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && description.Width >= 256 &&
			       description.Height >= 256 &&
			       (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
		}

		[[nodiscard]] bool CopyHeapSnapshot(
			ID3D12GraphicsCommandList* a_commandList,
			D3D12Renderer::DescriptorHeapSnapshot& a_snapshot) noexcept
		{
			if (!descriptorHeapState.Count || descriptorHeapState.CommandList.Get() != a_commandList) {
				return false;
			}
			a_snapshot = {};
			a_snapshot.Count = descriptorHeapState.Count;
			for (UINT index = 0; index < descriptorHeapState.Count; ++index) {
				a_snapshot.Heaps[index] = descriptorHeapState.Heaps[index].Get();
			}
			return true;
		}

		[[nodiscard]] ComPtr<ID3DBlob> CompileShader(
			const char* a_source,
			const char* a_target)
		{
			ComPtr<ID3DBlob> code;
			ComPtr<ID3DBlob> errors;
			const auto result = ::D3DCompile(
				a_source,
				std::strlen(a_source),
				nullptr,
				nullptr,
				nullptr,
				"main",
				a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3,
				0,
				code.GetAddressOf(),
				errors.GetAddressOf());
			if (FAILED(result)) {
				logger::critical(
					"FG compositor shader {} failed: {}",
					a_target,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				return {};
			}
			return code;
		}

		[[nodiscard]] bool InitializeCompositorStaticResources(
			CompositorState& a_state,
			ID3D12Device* a_device)
		{
			if (a_state.Failed) {
				return false;
			}
			if (a_state.Device) {
				return D3D12Renderer::HasSameDeviceIdentity(a_state.Device.Get(), a_device);
			}

			a_state.Device = a_device;

			D3D12_DESCRIPTOR_RANGE descriptorRange{};
			descriptorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			descriptorRange.NumDescriptors = 1;
			descriptorRange.BaseShaderRegister = 0;
			descriptorRange.OffsetInDescriptorsFromTableStart =
				D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

			D3D12_ROOT_PARAMETER rootParameter{};
			rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			rootParameter.DescriptorTable.NumDescriptorRanges = 1;
			rootParameter.DescriptorTable.pDescriptorRanges = &descriptorRange;
			rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC rootSignatureDescription{};
			rootSignatureDescription.NumParameters = 1;
			rootSignatureDescription.pParameters = &rootParameter;
			rootSignatureDescription.NumStaticSamplers = 1;
			rootSignatureDescription.pStaticSamplers = &sampler;

			ComPtr<ID3DBlob> rootSignatureBlob;
			ComPtr<ID3DBlob> rootSignatureErrors;
			if (FAILED(::D3D12SerializeRootSignature(
					&rootSignatureDescription,
					D3D_ROOT_SIGNATURE_VERSION_1,
					rootSignatureBlob.GetAddressOf(),
					rootSignatureErrors.GetAddressOf())) ||
				!rootSignatureBlob ||
				FAILED(a_device->CreateRootSignature(
					0,
					rootSignatureBlob->GetBufferPointer(),
					rootSignatureBlob->GetBufferSize(),
					IID_PPV_ARGS(a_state.RootSignature.GetAddressOf())))) {
				logger::critical("FG compositor root signature initialization failed");
				a_state.Failed = true;
				return false;
			}

			const auto vertexShader = CompileShader(fullscreenVertexShader, "vs_5_0");
			const auto pixelShader = CompileShader(compositePixelShader, "ps_5_0");
			if (!vertexShader || !pixelShader) {
				a_state.Failed = true;
				return false;
			}

			D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
			pipelineDescription.pRootSignature = a_state.RootSignature.Get();
			pipelineDescription.VS = {
				vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()
			};
			pipelineDescription.PS = {
				pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()
			};
			pipelineDescription.SampleMask = UINT_MAX;
			pipelineDescription.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pipelineDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pipelineDescription.RasterizerState.DepthClipEnable = TRUE;
			pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pipelineDescription.NumRenderTargets = 1;
			pipelineDescription.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
			pipelineDescription.SampleDesc.Count = 1;

			auto& blend = pipelineDescription.BlendState.RenderTarget[0];
			blend.BlendEnable = TRUE;
			blend.SrcBlend = D3D12_BLEND_ONE;
			blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOp = D3D12_BLEND_OP_ADD;
			blend.SrcBlendAlpha = D3D12_BLEND_ONE;
			blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

			if (FAILED(a_device->CreateGraphicsPipelineState(
					&pipelineDescription,
					IID_PPV_ARGS(a_state.Pipeline.GetAddressOf())))) {
				logger::critical("FG compositor pipeline initialization failed");
				a_state.Failed = true;
				return false;
			}

			D3D12_DESCRIPTOR_HEAP_DESC shaderHeapDescription{};
			shaderHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			shaderHeapDescription.NumDescriptors = 1;
			shaderHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(a_device->CreateDescriptorHeap(
					&shaderHeapDescription,
					IID_PPV_ARGS(a_state.ShaderHeap.GetAddressOf())))) {
				logger::critical("FG compositor SRV heap initialization failed");
				a_state.Failed = true;
				return false;
			}

			D3D12_DESCRIPTOR_HEAP_DESC renderTargetHeapDescription{};
			renderTargetHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			renderTargetHeapDescription.NumDescriptors = 2;
			if (FAILED(a_device->CreateDescriptorHeap(
					&renderTargetHeapDescription,
					IID_PPV_ARGS(a_state.RenderTargetHeap.GetAddressOf())))) {
				logger::critical("FG compositor RTV heap initialization failed");
				a_state.Failed = true;
				return false;
			}
			a_state.RenderTargetStride =
				a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			return true;
		}

		[[nodiscard]] bool EnsureOverlayTarget(
			CompositorState& a_state,
			std::uint64_t a_width,
			std::uint32_t a_height)
		{
			if (a_state.OverlayTarget && a_state.Width == a_width && a_state.Height == a_height) {
				return true;
			}
			if (a_state.OverlayTarget) {
				a_state.RetiredTargets.push_back(a_state.OverlayTarget);
				a_state.OverlayTarget.Reset();
			}

			D3D12_HEAP_PROPERTIES heapProperties{};
			heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
			heapProperties.CreationNodeMask = 1;
			heapProperties.VisibleNodeMask = 1;

			D3D12_RESOURCE_DESC resourceDescription{};
			resourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			resourceDescription.Width = a_width;
			resourceDescription.Height = a_height;
			resourceDescription.DepthOrArraySize = 1;
			resourceDescription.MipLevels = 1;
			resourceDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			resourceDescription.SampleDesc.Count = 1;
			resourceDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			resourceDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

			D3D12_CLEAR_VALUE clearValue{};
			clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			if (FAILED(a_state.Device->CreateCommittedResource(
					&heapProperties,
					D3D12_HEAP_FLAG_NONE,
					&resourceDescription,
					D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
					&clearValue,
					IID_PPV_ARGS(a_state.OverlayTarget.GetAddressOf())))) {
				logger::critical("FG compositor intermediate target creation failed");
				return false;
			}

			D3D12_SHADER_RESOURCE_VIEW_DESC shaderView{};
			shaderView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			shaderView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			shaderView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			shaderView.Texture2D.MipLevels = 1;
			a_state.Device->CreateShaderResourceView(
				a_state.OverlayTarget.Get(),
				&shaderView,
				a_state.ShaderHeap->GetCPUDescriptorHandleForHeapStart());

			D3D12_RENDER_TARGET_VIEW_DESC renderTargetView{};
			renderTargetView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			renderTargetView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			a_state.Device->CreateRenderTargetView(
				a_state.OverlayTarget.Get(),
				&renderTargetView,
				a_state.RenderTargetHeap->GetCPUDescriptorHandleForHeapStart());

			a_state.Width = a_width;
			a_state.Height = a_height;
			if (!a_state.ActiveLogged) {
				a_state.ActiveLogged = true;
				logger::info("FG compositor experiment active: ImGui -> RGBA8 layer -> Scaleform target");
			}
			return true;
		}

		[[nodiscard]] bool CompositeThroughIntermediate(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12Resource* a_renderTarget,
			const D3D12Renderer::DescriptorHeapSnapshot& a_engineHeaps)
		{
			const auto targetDescription = a_renderTarget->GetDesc();
			if (!IsRgba8Format(targetDescription.Format)) {
				return false;
			}

			ComPtr<ID3D12Device> device;
			if (FAILED(a_commandList->GetDevice(IID_PPV_ARGS(device.GetAddressOf())))) {
				return false;
			}

			std::scoped_lock lock{ GetCompositorMutex() };
			auto& compositor = GetCompositorState();
			if (!InitializeCompositorStaticResources(compositor, device.Get()) ||
				!EnsureOverlayTarget(
					compositor,
					targetDescription.Width,
					targetDescription.Height)) {
				return false;
			}

			D3D12_RESOURCE_BARRIER toRenderTarget{};
			toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			toRenderTarget.Transition.pResource = compositor.OverlayTarget.Get();
			toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			a_commandList->ResourceBarrier(1, &toRenderTarget);

			const auto overlayRtv =
				compositor.RenderTargetHeap->GetCPUDescriptorHandleForHeapStart();
			constexpr float transparent[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
			a_commandList->ClearRenderTargetView(overlayRtv, transparent, 0, nullptr);

			D3D12Renderer::Render(
				a_commandList,
				compositor.OverlayTarget.Get(),
				a_engineHeaps,
				setDescriptorHeapsOriginal.load(std::memory_order_acquire));

			D3D12_RESOURCE_BARRIER toShaderResource = toRenderTarget;
			toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			a_commandList->ResourceBarrier(1, &toShaderResource);

			auto gameRtv = compositor.RenderTargetHeap->GetCPUDescriptorHandleForHeapStart();
			gameRtv.ptr += compositor.RenderTargetStride;
			D3D12_RENDER_TARGET_VIEW_DESC gameRenderTargetView{};
			gameRenderTargetView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			gameRenderTargetView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			compositor.Device->CreateRenderTargetView(
				a_renderTarget,
				&gameRenderTargetView,
				gameRtv);

			ID3D12DescriptorHeap* compositorHeaps[]{ compositor.ShaderHeap.Get() };
			setDescriptorHeapsOriginal.load(std::memory_order_acquire)(
				a_commandList, 1, compositorHeaps);
			a_commandList->SetGraphicsRootSignature(compositor.RootSignature.Get());
			a_commandList->SetPipelineState(compositor.Pipeline.Get());
			a_commandList->SetGraphicsRootDescriptorTable(
				0,
				compositor.ShaderHeap->GetGPUDescriptorHandleForHeapStart());

			const D3D12_VIEWPORT viewport{
				0.0f,
				0.0f,
				static_cast<float>(targetDescription.Width),
				static_cast<float>(targetDescription.Height),
				0.0f,
				1.0f
			};
			const D3D12_RECT scissor{
				0,
				0,
				static_cast<LONG>(targetDescription.Width),
				static_cast<LONG>(targetDescription.Height)
			};
			a_commandList->RSSetViewports(1, &viewport);
			a_commandList->RSSetScissorRects(1, &scissor);
			a_commandList->OMSetRenderTargets(1, &gameRtv, FALSE, nullptr);
			a_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			a_commandList->DrawInstanced(3, 1, 0, 0);

			setDescriptorHeapsOriginal.load(std::memory_order_acquire)(
				a_commandList,
				a_engineHeaps.Count,
				a_engineHeaps.Heaps.data());
			return true;
		}

		void RenderCandidate(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12Resource* a_renderTarget,
			const D3D12Renderer::DescriptorHeapSnapshot& a_engineHeaps)
		{
			internalD3D = true;
			const bool composited = CompositeThroughIntermediate(
				a_commandList,
				a_renderTarget,
				a_engineHeaps);
			if (!composited) {
				if (IsRgba8Format(a_renderTarget->GetDesc().Format) &&
					!compositorFallbackLogged.exchange(true, std::memory_order_relaxed)) {
					logger::critical("FG compositor experiment unavailable; using direct ImGui rendering");
				}
				D3D12Renderer::Render(
					a_commandList,
					a_renderTarget,
					a_engineHeaps,
					setDescriptorHeapsOriginal.load(std::memory_order_acquire));
			}
			internalD3D = false;
		}

		void InspectBarrierCandidate(
			ID3D12GraphicsCommandList* a_commandList,
			const D3D12_RESOURCE_BARRIER& a_barrier) noexcept
		{
			if (!regionState.Active || a_barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
				a_barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE ||
				a_barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET ||
				!IsCandidateResource(a_barrier.Transition.pResource)) {
				return;
			}

			const auto stateAfter = a_barrier.Transition.StateAfter;
			const bool ordinaryTarget =
				(stateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
			const bool copyTarget =
				(stateAfter & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0 && !ordinaryTarget;
			if (!ordinaryTarget && !copyTarget) {
				return;
			}
			if ((ordinaryTarget && regionState.SawOrdinaryTarget) ||
				(copyTarget && regionState.SawCopyTarget)) {
				return;
			}

			regionState.SawOrdinaryTarget = regionState.SawOrdinaryTarget || ordinaryTarget;
			regionState.SawCopyTarget = regionState.SawCopyTarget || copyTarget;
			if (regionState.FrameStarted) {
				return;
			}

			const bool selected =
				(regionState.Previous == PreviousRegion::Unknown && copyTarget) ||
				(regionState.Previous == PreviousRegion::Normal && ordinaryTarget) ||
				(regionState.Previous == PreviousRegion::FrameGeneration && copyTarget);
			if (!selected) {
				return;
			}
			regionState.FrameStarted = true;

			D3D12Renderer::DescriptorHeapSnapshot heapSnapshot;
			if (!CopyHeapSnapshot(a_commandList, heapSnapshot)) {
				return;
			}
			RenderCandidate(a_commandList, a_barrier.Transition.pResource, heapSnapshot);
		}

		void STDMETHODCALLTYPE ResourceBarrierThunk(
			ID3D12GraphicsCommandList* a_commandList,
			UINT a_barrierCount,
			const D3D12_RESOURCE_BARRIER* a_barriers) noexcept
		{
			const auto original = resourceBarrierOriginal.load(std::memory_order_acquire);
			if (internalD3D) {
				selfTestSeen |= resourceBarrierSeen;
				original(a_commandList, a_barrierCount, a_barriers);
				return;
			}

			if (state.load(std::memory_order_acquire) == HookState::Ready &&
				regionState.Active && a_commandList &&
				a_commandList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT && a_barriers) {
				const bool sawCandidate = regionState.SawOrdinaryTarget || regionState.SawCopyTarget;
				if (!sawCandidate || regionState.BarrierCallsAfterFirstCandidate < 4) {
					regionState.BarrierCallsAfterFirstCandidate += sawCandidate;
					for (UINT index = 0; index < a_barrierCount; ++index) {
						InspectBarrierCandidate(a_commandList, a_barriers[index]);
					}
				}
			}
			original(a_commandList, a_barrierCount, a_barriers);
		}

		void InvalidateHeapCapture(ID3D12GraphicsCommandList* a_commandList) noexcept
		{
			if (descriptorHeapState.CommandList.Get() == a_commandList) {
				descriptorHeapState = {};
			}
		}

		HRESULT STDMETHODCALLTYPE ResetThunk(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12CommandAllocator* a_allocator,
			ID3D12PipelineState* a_initialState) noexcept
		{
			const auto original = resetOriginal.load(std::memory_order_acquire);
			if (internalD3D) {
				selfTestSeen |= resetSeen;
				return original(a_commandList, a_allocator, a_initialState);
			}
			const auto result = original(a_commandList, a_allocator, a_initialState);
			if (SUCCEEDED(result)) {
				InvalidateHeapCapture(a_commandList);
			}
			return result;
		}

		void STDMETHODCALLTYPE ClearStateThunk(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12PipelineState* a_pipelineState) noexcept
		{
			const auto original = clearStateOriginal.load(std::memory_order_acquire);
			if (internalD3D) {
				selfTestSeen |= clearStateSeen;
				original(a_commandList, a_pipelineState);
				return;
			}
			original(a_commandList, a_pipelineState);
			InvalidateHeapCapture(a_commandList);
		}

		void STDMETHODCALLTYPE SetDescriptorHeapsThunk(
			ID3D12GraphicsCommandList* a_commandList,
			UINT a_heapCount,
			ID3D12DescriptorHeap* const* a_heaps) noexcept
		{
			const auto original = setDescriptorHeapsOriginal.load(std::memory_order_acquire);
			original(a_commandList, a_heapCount, a_heaps);
			if (internalD3D) {
				selfTestSeen |= descriptorHeapsSeen;
				return;
			}

			DescriptorHeapState nextState;
			if (!a_commandList || a_commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
				!a_heaps || a_heapCount == 0 || a_heapCount > nextState.Heaps.size()) {
				descriptorHeapState = {};
				return;
			}
			for (UINT index = 0; index < a_heapCount; ++index) {
				if (!a_heaps[index]) {
					descriptorHeapState = {};
					return;
				}
				nextState.Heaps[index] = a_heaps[index];
			}
			nextState.CommandList = a_commandList;
			nextState.Count = a_heapCount;
			descriptorHeapState = std::move(nextState);
		}
	}

	bool EnsureInstalled() noexcept
	{
		auto current = state.load(std::memory_order_acquire);
		if (current == HookState::Ready) {
			return true;
		}
		if (current == HookState::Installing || current == HookState::Failed) {
			return false;
		}

		HookState expected = HookState::Uninitialized;
		if (!state.compare_exchange_strong(
				expected,
				HookState::Installing,
				std::memory_order_acq_rel)) {
			return expected == HookState::Ready;
		}

		auto fail = []() noexcept {
			state.store(HookState::Failed, std::memory_order_release);
			return false;
		};

		auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
		if (!renderer || !renderer->GetDevice()) {
			state.store(HookState::Uninitialized, std::memory_order_release);
			return false;
		}

		ComPtr<ID3D12Device> device;
		auto* borrowedDevice = reinterpret_cast<ID3D12Device*>(renderer->GetDevice());
		if (FAILED(borrowedDevice->QueryInterface(IID_PPV_ARGS(device.GetAddressOf())))) {
			return fail();
		}

		ComPtr<ID3D12CommandAllocator> allocator;
		if (FAILED(device->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(allocator.GetAddressOf())))) {
			return fail();
		}

		ComPtr<ID3D12GraphicsCommandList> commandList;
		if (FAILED(device->CreateCommandList(
				0,
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				allocator.Get(),
				nullptr,
				IID_PPV_ARGS(commandList.GetAddressOf())))) {
			return fail();
		}

		ID3D12GraphicsCommandList* native{};
		if (FAILED(commandList->QueryInterface(
				streamlineNativeInterface,
				reinterpret_cast<void**>(&native))) || !native) {
			return fail();
		}
		ComPtr<ID3D12GraphicsCommandList> nativeCommandList;
		nativeCommandList.Attach(native);
		if (native == commandList.Get()) {
			return fail();
		}

		ComPtr<ID3D12Device> hookDevice;
		if (FAILED(commandList->GetDevice(IID_PPV_ARGS(hookDevice.GetAddressOf()))) ||
			!D3D12Renderer::HasSameDeviceIdentity(hookDevice.Get(), device.Get())) {
			logger::critical("The Streamline proxy command list and renderer device do not match");
			return fail();
		}

		auto* rawVtable = *reinterpret_cast<std::uintptr_t**>(commandList.Get());
		if (!rawVtable) {
			return fail();
		}
		REL::Relocation<std::uintptr_t> vtable{ reinterpret_cast<std::uintptr_t>(rawVtable) };
		std::array<std::uintptr_t, commandListSlots.size()> targets{};
		for (std::size_t index = 0; index < commandListSlots.size(); ++index) {
			targets[index] = Detail::ReadVtableSlot(vtable, commandListSlots[index]);
			if (!IsStreamlineTarget(targets[index])) {
				return fail();
			}
		}

		const std::array<std::uintptr_t, commandListSlots.size()> replacements{
			Detail::FunctionAddress(&ResetThunk),
			Detail::FunctionAddress(&ClearStateThunk),
			Detail::FunctionAddress(&ResourceBarrierThunk),
			Detail::FunctionAddress(&SetDescriptorHeapsThunk)
		};
		for (std::size_t index = 0; index < targets.size(); ++index) {
			if (targets[index] == replacements[index]) {
				return fail();
			}
		}

		resetOriginal.store(reinterpret_cast<ResetFunction>(targets[0]), std::memory_order_release);
		clearStateOriginal.store(reinterpret_cast<ClearStateFunction>(targets[1]), std::memory_order_release);
		resourceBarrierOriginal.store(
			reinterpret_cast<ResourceBarrierFunction>(targets[2]), std::memory_order_release);
		setDescriptorHeapsOriginal.store(
			reinterpret_cast<SetDescriptorHeapsFunction>(targets[3]), std::memory_order_release);

		std::array<Detail::VtableHook, 4> hooks{
			Detail::VtableHook{ &vtable, commandListSlots[0], targets[0], replacements[0] },
			Detail::VtableHook{ &vtable, commandListSlots[1], targets[1], replacements[1] },
			Detail::VtableHook{ &vtable, commandListSlots[2], targets[2], replacements[2] },
			Detail::VtableHook{ &vtable, commandListSlots[3], targets[3], replacements[3] }
		};
		if (!Detail::CommitHooks(hooks)) {
			static_cast<void>(Detail::RollBackHooks(hooks));
			return fail();
		}

		selfTestSeen = 0;
		internalD3D = true;
		D3D12_RESOURCE_BARRIER testBarrier{};
		testBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		testBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		testBarrier.UAV.pResource = nullptr;
		commandList->ResourceBarrier(1, &testBarrier);
		commandList->SetDescriptorHeaps(0, nullptr);
		commandList->ClearState(nullptr);
		const auto closeResult = commandList->Close();
		const auto resetResult = SUCCEEDED(closeResult) ?
			commandList->Reset(allocator.Get(), nullptr) : E_FAIL;
		const auto finalCloseResult = SUCCEEDED(resetResult) ? commandList->Close() : E_FAIL;
		internalD3D = false;

		if (selfTestSeen != allSelfTestsSeen || FAILED(closeResult) || FAILED(resetResult) ||
			FAILED(finalCloseResult)) {
			static_cast<void>(Detail::RollBackHooks(hooks));
			return fail();
		}

		internalD3D = true;
		const bool rendererReady = D3D12Renderer::Initialize(hookDevice.Get());
		internalD3D = false;
		if (!rendererReady) {
			static_cast<void>(Detail::RollBackHooks(hooks));
			logger::critical("The ImGui renderer could not initialize through Streamline");
			return fail();
		}

		state.store(HookState::Ready, std::memory_order_release);
		logger::info("NVIDIA Streamline command-list proxy detected; rendering through its proxy interface");
		return true;
	}

	void ResetRegion() noexcept
	{
		if (regionState.Active && (regionState.SawOrdinaryTarget || regionState.SawCopyTarget)) {
			previousRegion.store(
				regionState.SawCopyTarget ?
					PreviousRegion::FrameGeneration :
					PreviousRegion::Normal,
				std::memory_order_release);
		}
		regionState = {};
	}

	void ActivateRegion() noexcept
	{
		regionState = {};
		regionState.Active = true;
		regionState.Previous = previousRegion.load(std::memory_order_acquire);
	}
}
