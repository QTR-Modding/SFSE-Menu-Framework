#include "rendering/PresentOverlay.h"

#include "platform/win32/Win32PlatformInternal.h"
#include "rendering/D3D12Renderer.h"
#include "rendering/StreamlineUIPrototype.h"
#include "rendering/CommandListState.h"
#include "rendering/FramePresentBridge.h"
#include "rendering/OverlayCompositor.h"
#include "rendering/D3D12Texture.h"

#include <RE/C/CreationRenderer.h>

#include <Windows.h>
#include <d3d12.h>
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

		constexpr GUID streamlineNative{
			0xADEC44E2, 0x61F0, 0x45C3,
			{ 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF }
		};

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

			OverlayCompositor::Shaders Shaders;
			ComPtr<ID3D12DescriptorHeap> SrvHeap;
			ComPtr<ID3D12DescriptorHeap> RtvHeap;

			ComPtr<ID3D12Resource> Overlay;
			bool OverlayInitialized{};
			std::uint64_t OverlayGeneration{};
			std::uint64_t Width{};
			std::uint32_t Height{};
			ComPtr<ID3D12PipelineState> Pipeline;
			DXGI_FORMAT PipelineFormat{ DXGI_FORMAT_UNKNOWN };
			bool Initialized{};
			bool SubmissionFailed{};
		};

		std::atomic<HookState> hookState{ HookState::Uninitialized };
		std::atomic_flag deviceChangeLogged{};
		std::atomic_flag queueChangeLogged{};

		[[nodiscard]] State& GetState()
		{
			static auto* state = new State();
			return *state;
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

		using D3D12Renderer::HasSameIdentity;

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

		using OverlayCompositor::RtvFormat;
		using OverlayCompositor::Transition;

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
						a_state.Frames.resize(oldSize);
						return false;
					}
				}
			}
			if (!a_state.List) {
				if (FAILED(a_state.Device->CreateCommandList(
						0, D3D12_COMMAND_LIST_TYPE_DIRECT, a_state.Frames[0].Allocator.Get(),
						nullptr, IID_PPV_ARGS(a_state.List.GetAddressOf()))) ||
					FAILED(a_state.List->Close())) {
					a_state.List.Reset();
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
			a_state.OverlayInitialized = false;
			a_state.OverlayGeneration = 0;

			if (!OverlayCompositor::CreateTexture(a_state.Device.Get(), a_width, a_height,
				a_state.SrvHeap.Get(), a_state.Overlay)) {
				return false;
			}

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

			if (!OverlayCompositor::CreatePipeline(a_state.Device.Get(), a_state.Shaders, a_format, a_state.Pipeline)) {
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
				if (!HasSameIdentity(a_state.Device.Get(), a_device)) {
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
				!HasSameIdentity(a_device, queueDevice.Get()) ||
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

			Transition(state.List.Get(), state.Overlay.Get(),
				D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

			auto generation = state.OverlayGeneration;
			const bool recorded = D3D12Renderer::Render(
				state.List.Get(), state.Overlay.Get(), generation);

			Transition(state.List.Get(), state.Overlay.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

			// A skipped draw keeps the last submitted image, if one exists.
			if (recorded || state.OverlayInitialized) {
				Transition(state.List.Get(), backBuffer.Get(),
					D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
				const auto finalRtv = D3D12Textures::RenderTargetView(
					state.Device.Get(), state.RtvHeap.Get(), backBuffer.Get(), finalFormat);

				OverlayCompositor::Draw(state.List.Get(), state.Shaders, state.Pipeline.Get(),
					state.SrvHeap.Get(), finalRtv, backDesc.Width, backDesc.Height);

				Transition(state.List.Get(), backBuffer.Get(),
					D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
			}
			if (FAILED(state.List->Close())) {
				return;
			}

			ID3D12CommandList* lists[]{ state.List.Get() };
			state.Queue->ExecuteCommandLists(1, lists);
			state.OverlayInitialized = state.OverlayInitialized || recorded;
			state.OverlayGeneration = generation;
			const auto fenceValue = state.NextFence++;
			if (FAILED(state.Queue->Signal(state.Fence.Get(), fenceValue))) {
				state.SubmissionFailed = true;
				logger::critical("DXGI Present overlay fence signal failed; rendering is disabled");
				return;
			}
			frame.FenceValue = fenceValue;
			if (state.OverlayInitialized) {
				D3D12Renderer::NotifyOverlayComposited(state.OverlayGeneration);
			}
		}

		HRESULT STDMETHODCALLTYPE PresentFrame(
			IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags, std::uint32_t a_frame) noexcept
		{
			// This call belongs to the game's request, not a generated native frame.
			// Keep the original swapchain/proxy chain intact.
			if (!(a_flags & DXGI_PRESENT_TEST)) {
				const bool hasUI = StreamlineUIPrototype::HasUIRenderForFrame(a_frame);
				if (!hasUI) { Draw(a_swapChain); }
			}
			return a_swapChain->Present(a_sync, a_flags);
		}

		[[nodiscard]] bool Install()
		{
			const auto execute = RE::CreationRendererPrivate::PresentRequest::Execute.address();
			const auto site = execute + 0x100;
			constexpr std::uint8_t prologue[]{ 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40 };
			constexpr std::uint8_t requestRegister[]{ 0x4C, 0x8B, 0xF9 }; // mov r15,rcx
			constexpr std::uint8_t arguments[]{ 0x48, 0x8B, 0x4F, 0x40, 0x44, 0x8B, 0x47, 0x54, 0x8B, 0x57, 0x50 };
			constexpr std::uint8_t original[]{ 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x40 }; // mov rax,[rcx]; call [rax+40h]
			const auto text = REX::FModule::GetExecutingModule().GetSection(".text");
			const auto begin = reinterpret_cast<std::uintptr_t>(text.GetPointer<std::byte>());
			if (execute < begin || text.GetSize() < 0x106 || execute - begin > text.GetSize() - 0x106 ||
				std::memcmp(reinterpret_cast<void*>(execute), prologue, sizeof(prologue)) != 0 ||
				std::memcmp(reinterpret_cast<void*>(execute + 0x0D), requestRegister, sizeof(requestRegister)) != 0 ||
				std::memcmp(reinterpret_cast<void*>(site - sizeof(arguments)), arguments, sizeof(arguments)) != 0 ||
				std::memcmp(reinterpret_cast<void*>(site), original, sizeof(original)) != 0) {
				logger::critical("Game-frame Present hook signature mismatch; no code changed");
				return false;
			}

			static auto* trampoline = new REL::Trampoline("SFSE-MF frame presentation");
			trampoline->create(64, reinterpret_cast<void*>(site));
			auto* bridge = trampoline->allocate<FramePresentBridge>(reinterpret_cast<std::uintptr_t>(&PresentFrame));
			const auto destination = reinterpret_cast<std::uintptr_t>(bridge);
			const auto relay = trampoline->allocate_branch6(destination);
			const auto displacement = static_cast<std::int64_t>(relay) - static_cast<std::int64_t>(site + 6);
			if (displacement < INT32_MIN || displacement > INT32_MAX) {
				logger::critical("Game-frame Present relay is out of reach");
				return false;
			}
			if (!::FlushInstructionCache(::GetCurrentProcess(), bridge, sizeof(*bridge))) { return false; }

			// CommonLib emits the CALL6. Its inferred original target is unused:
			// the replaced six bytes are a MOV plus an indirect virtual CALL.
			static_cast<void>(trampoline->write_call<6>(site, destination));
			const REL::ASM::CALL6 expected{ site, relay };
			if (std::memcmp(reinterpret_cast<void*>(site), &expected, sizeof(expected)) != 0 ||
				!::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(expected))) {
				REL::WriteSafeData(site, original);
				if (std::memcmp(reinterpret_cast<void*>(site), original, sizeof(original)) != 0 ||
					!::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(original))) {
					REX::FAIL("Could not restore the game-frame Present hook");
				}
				return false;
			}
			logger::info("Game-frame Present routing installed; no elapsed-time fallback");
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
