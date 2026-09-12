#include "rendering/OverlayCompositor.h"
#include "rendering/D3D12Texture.h"
#include <cstring>

namespace SFSEMenuFramework::OverlayCompositor
{
	using Microsoft::WRL::ComPtr;
	using SerializeRootFn = HRESULT(WINAPI*)(
		const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
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

	[[nodiscard]] static ComPtr<ID3DBlob> Compile(const char* a_source, const char* a_target)
	{
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> errors;
		if (FAILED(::D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr, nullptr, "main", a_target,
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf()))) {
			logger::critical("Overlay compositor shader {} failed: {}", a_target,
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
			return {};
		}
		return code;
	}

	[[nodiscard]] static SerializeRootFn RootSerializer()
	{
		static const auto serializer = []() -> SerializeRootFn {
			const auto module = ::LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			return module ? reinterpret_cast<SerializeRootFn>(
								::GetProcAddress(module, "D3D12SerializeRootSignature"))
						  : nullptr;
		}();
		return serializer;
	}

	bool CreateShaders(ID3D12Device* device, Shaders& shaders)
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
				&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, rootBlob.GetAddressOf(), errors.GetAddressOf())) ||
			!rootBlob ||
			FAILED(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
				IID_PPV_ARGS(shaders.RootSignature.GetAddressOf())))) {
			return false;
		}

		shaders.VS = Compile(vertexShader, "vs_5_0");
		shaders.PS = Compile(pixelShader, "ps_5_0");
		if (!shaders.VS || !shaders.PS) {
			return false;
		}

		return true;
	}

	bool CreatePipeline(ID3D12Device* device, const Shaders& shaders, DXGI_FORMAT a_format,
		ComPtr<ID3D12PipelineState>& pipeline)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = shaders.RootSignature.Get();
		desc.VS = {shaders.VS->GetBufferPointer(), shaders.VS->GetBufferSize()};
		desc.PS = {shaders.PS->GetBufferPointer(), shaders.PS->GetBufferSize()};
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

		if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipeline.GetAddressOf())))) {
			return false;
		}

		return true;
	}

	bool CreateTexture(ID3D12Device* device, std::uint64_t a_width, std::uint32_t a_height,
		ID3D12DescriptorHeap* srvHeap, ComPtr<ID3D12Resource>& overlay)
	{
		const auto heap = D3D12Textures::HeapProperties(D3D12_HEAP_TYPE_DEFAULT);

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = a_width;
		desc.Height = a_height;
		desc.DepthOrArraySize = desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(overlay.GetAddressOf())))) {
			return false;
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(overlay.Get(), &srv, srvHeap->GetCPUDescriptorHandleForHeapStart());
		return true;
	}

	void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept
	{
		if (!list || !resource || before == after) { return; }
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
		list->ResourceBarrier(1, &barrier);
	}

	void Draw(ID3D12GraphicsCommandList* list, const Shaders& shaders, ID3D12PipelineState* pipeline,
		ID3D12DescriptorHeap* heap, D3D12_CPU_DESCRIPTOR_HANDLE targetRtv, std::uint64_t width,
		std::uint32_t height)
	{
		ID3D12DescriptorHeap* heaps[]{heap};
		list->SetDescriptorHeaps(1, heaps);
		list->SetGraphicsRootSignature(shaders.RootSignature.Get());
		list->SetPipelineState(pipeline);
		list->SetGraphicsRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());

		const D3D12_VIEWPORT viewport{
			0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
		const D3D12_RECT scissor{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
		list->RSSetViewports(1, &viewport);
		list->RSSetScissorRects(1, &scissor);
		list->OMSetRenderTargets(1, &targetRtv, FALSE, nullptr);
		list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		list->DrawInstanced(3, 1, 0, 0);
	}
} // namespace SFSEMenuFramework::OverlayCompositor
