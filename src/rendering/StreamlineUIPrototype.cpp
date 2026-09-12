#include "rendering/StreamlineUIPrototype.h"

#include "rendering/D3D12Renderer.h"
#include "rendering/CommandListState.h"
#include "rendering/GpuResourceRetirement.h"
#include "rendering/FrameRouteHistory.h"
#include "rendering/OverlayCompositor.h"
#include "rendering/D3D12Texture.h"

#include <RE/C/CreationRenderer.h>

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

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

		constexpr std::uint32_t uiColorAndAlpha = 23;
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

		struct State final
		{
			ComPtr<ID3D12Device> Device;
			ComPtr<ID3D12DescriptorHeap> SrvHeap;
			ComPtr<ID3D12DescriptorHeap> RtvHeap;
			OverlayCompositor::Shaders Shaders;
			ComPtr<ID3D12PipelineState> Pipeline;
			ComPtr<ID3D12Resource> Overlay;
			GpuResourceRetirement Retirement;
			std::uint64_t Width{};
			std::uint32_t Height{};
			DXGI_FORMAT PipelineFormat{ DXGI_FORMAT_UNKNOWN };
			bool Initialized{};
			bool HasOverlayContent{};
			bool Failed{};
		};

		std::atomic<SetTagFn> originalSetTag{};
		std::atomic_flag installed{};
		std::atomic_flag rejectionLogged{};
		std::atomic_flag heapWaitLogged{};
		std::atomic_flag routeLogged{};
		FrameRouteHistory frameRoutes;

		thread_local ID3D12GraphicsCommandList* lastRenderedCommandList{};
		thread_local std::uint64_t lastRenderedEpoch{};

		[[nodiscard]] State& GetState()
		{
			static auto* state = new State();
			return *state;
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

		using D3D12Renderer::HasSameIdentity;

		template <class T>
		[[nodiscard]] bool ResolveNative(T* a_object, ComPtr<T>& a_native) noexcept
		{
			a_native.Reset();
			if (!a_object) { return false; }
			T* native{};
			if (SUCCEEDED(a_object->QueryInterface(streamlineNativeInterface,
				reinterpret_cast<void**>(&native))) && native) {
				a_native.Attach(native);
			} else {
				a_native = a_object;
			}
			return true;
		}
		using OverlayCompositor::RtvFormat;

		[[nodiscard]] bool CreateStaticResources(State& a_state)
		{
			if (!OverlayCompositor::CreateShaders(a_state.Device.Get(), a_state.Shaders)) {
				return false;
			}

			if (!D3D12Textures::CreateDescriptorHeap(a_state.Device.Get(),
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, a_state.SrvHeap) ||
				!D3D12Textures::CreateDescriptorHeap(a_state.Device.Get(),
					D3D12_DESCRIPTOR_HEAP_TYPE_RTV, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, a_state.RtvHeap)) {
				return false;
			}
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
				ComPtr<ID3D12DescriptorHeap> nextHeap;
				if (!D3D12Textures::CreateDescriptorHeap(a_state.Device.Get(),
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, nextHeap)) {
					return false;
				}
				// Old command lists still reference the old GPU descriptor after a resize.
				a_state.SrvHeap = std::move(nextHeap);
				a_state.Overlay.Reset();
			}

			if (!OverlayCompositor::CreateTexture(a_state.Device.Get(), a_width, a_height,
				a_state.SrvHeap.Get(), a_state.Overlay)) {
				return false;
			}

			a_state.Width = a_width;
			a_state.Height = a_height;
			a_state.HasOverlayContent = false;
			return true;
		}

		[[nodiscard]] bool EnsurePipeline(State& a_state, DXGI_FORMAT a_format)
		{
			if (a_state.Pipeline && a_state.PipelineFormat == a_format) {
				return true;
			}
			if (a_state.Pipeline) {
				a_state.Pipeline.Reset();
			}

			if (!OverlayCompositor::CreatePipeline(a_state.Device.Get(), a_state.Shaders, a_format, a_state.Pipeline)) {
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
				logger::info("Streamline UI overlay renderer initialized");
			} else if (!HasSameIdentity(a_state.Device.Get(), a_device)) {
				LogRejection("UI resource device changed");
				return false;
			}

			return EnsureOverlay(a_state, a_width, a_height) && EnsurePipeline(a_state, a_format);
		}

		using OverlayCompositor::Transition;

		[[nodiscard]] bool RenderFramework(
			const ResourceTag& a_tag,
			void* a_commandBuffer) noexcept
		{
			if (!a_tag.ResourceData || !a_tag.ResourceData->Native) {
				return false;
			}
			std::scoped_lock routingLock{ CommandListState::RoutingMutex() };
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
			if (!ResolveNative(proxyList.Get(), nativeList) || !nativeList) {
				LogRejection("could not resolve native D3D12 command list");
				return false;
			}

			ComPtr<ID3D12Device> targetDevice;
			ComPtr<ID3D12Device> listDevice;
			ComPtr<ID3D12Device> nativeListDevice;
			if (FAILED(target->GetDevice(IID_PPV_ARGS(targetDevice.GetAddressOf()))) ||
				FAILED(proxyList->GetDevice(IID_PPV_ARGS(listDevice.GetAddressOf()))) ||
				!targetDevice || !listDevice ||
				!ResolveNative(listDevice.Get(), nativeListDevice) ||
				!HasSameIdentity(targetDevice.Get(), nativeListDevice.Get())) {
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
				if (!heapWaitLogged.test_and_set(std::memory_order_relaxed)) {
					logger::info("Streamline UI overlay waiting for complete command-list state: {}", stateReason);
				}
				return false;
			}
			const auto epoch = savedState.Epoch;
			if (lastRenderedCommandList == proxyList.Get() && lastRenderedEpoch == epoch) {
				return true;
			}

			auto& state = GetState();
			if (!EnsureRenderer(state, targetDevice.Get(), desc.Width, desc.Height, targetFormat)) {
				return false;
			}

			CommandListState::InjectionScope injection;
			ComPtr<ID3D12GraphicsCommandList2> markerList;
			if (FAILED(nativeList.As(&markerList))) { return false; }
			auto* use = state.Retirement.Begin(state.Device.Get(), state.Overlay.Get(),
				state.SrvHeap.Get(), state.Pipeline.Get());
			if (!use) {
				LogRejection("compositor completion slots unavailable");
				return false;
			}
			struct CompleteOnExit {
				GpuResourceRetirement::Use& Use;
				ID3D12GraphicsCommandList2* List;
				~CompleteOnExit() { GpuResourceRetirement::End(Use, List); }
			} completion{ *use, markerList.Get() };
			struct RestoreOnExit {
				ID3D12GraphicsCommandList* List;
				const CommandListState::Snapshot& Saved;
				~RestoreOnExit() { Saved.Restore(List); }
			} restore{ nativeList.Get(), savedState };
			Transition(
				nativeList.Get(), state.Overlay.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			const bool recorded = D3D12Renderer::Render(
				nativeList.Get(), state.Overlay.Get());
			Transition(
				nativeList.Get(), state.Overlay.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET,
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			state.HasOverlayContent |= recorded;
			// Reuse the last image on a busy frame, but never sample an uninitialized image.
			if (!state.HasOverlayContent) { return false; }
			const auto targetRtv = D3D12Textures::RenderTargetView(
				state.Device.Get(), state.RtvHeap.Get(), target.Get(), targetFormat);

			const auto originalState =
				static_cast<D3D12_RESOURCE_STATES>(a_tag.ResourceData->State);
			Transition(
				nativeList.Get(), target.Get(), originalState,
				D3D12_RESOURCE_STATE_RENDER_TARGET);

			OverlayCompositor::Draw(nativeList.Get(), state.Shaders, state.Pipeline.Get(),
				state.SrvHeap.Get(), targetRtv, desc.Width, desc.Height);

			Transition(
				nativeList.Get(), target.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, originalState);
			lastRenderedCommandList = proxyList.Get();
			lastRenderedEpoch = epoch;
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

					const auto frame = RE::CreationRendererPrivate::Renderer::GetRenderFrameIndex();
					frameRoutes.Record(frame, RenderFramework(tag, a_commandBuffer));
					break;
				}
			}

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

	bool HasUIRenderForFrame(std::uint32_t a_frame) noexcept
	{
		return frameRoutes.HasUI(a_frame);
	}
}
