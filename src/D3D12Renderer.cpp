#include "D3D12Renderer.h"

#include "Win32Platform.h"
#include "WindowManager.h"

#include <backends/imgui_impl_dx12.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <mutex>
#include <type_traits>

#include <wrl/client.h>

namespace SFSEMenuFramework::D3D12Renderer
{
	namespace
	{
		// SKSE Menu Framework 3 defaults FontSizeMedium to 32 px and builds
		// that font into its atlas (commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775, GPL-3.0). This port uses
		// ImGui's MIT-licensed embedded font at the same size until the full
		// configurable SFSE font loader is ported.
		constexpr float defaultFontSizePixels = 32.0F;
		using Microsoft::WRL::ComPtr;
		constexpr std::size_t frameResourceCount = 4;
		constexpr std::uint64_t maximumMainWindowFrameAgeMilliseconds = 250;
		constexpr char imguiIniFilename[] =
			"Data/SFSE/Plugins/SFSEMenuFramework.imgui.ini";

		std::atomic<std::uint64_t> renderedMainWindowGeneration{ 0 };
		std::atomic<std::uint64_t> lastMainWindowRenderTick{ 0 };
		std::atomic<std::uint64_t> nextContextGeneration{ 1 };
		std::atomic<bool>          rendererReady{ false };

		struct CompletionSlot final
		{
			std::uint32_t LastCompletedValue{ 0 };
			std::uint32_t PendingValue{ 0 };
			bool          Pending{ false };
		};

		struct RendererState final
		{
			ComPtr<ID3D12Device>                           Device;
			ComPtr<ID3D12DescriptorHeap>                   ShaderHeap;
			ComPtr<ID3D12DescriptorHeap>                   RenderTargetHeap;
			ComPtr<ID3D12Resource>                         CompletionBuffer;
			std::array<CompletionSlot, frameResourceCount> CompletionSlots{};
			std::uint64_t                                  NextFrameIndex{ 0 };
			ImGuiContext*                                  Context{ nullptr };
			std::uint64_t                                  ContextGeneration{ 0 };
			bool                                           InitializationFailed{ false };
		};

		[[nodiscard]] RendererState& GetRendererState()
		{
			static auto* state = new RendererState();
			return *state;
		}

		[[nodiscard]] std::recursive_mutex& GetRendererMutex()
		{
			static auto* mutex = new std::recursive_mutex();
			return *mutex;
		}

		[[nodiscard]] bool HasSameComIdentity(IUnknown* a_left, IUnknown* a_right)
		{
			if (!a_left || !a_right) {
				return false;
			}

			ComPtr<IUnknown> leftIdentity;
			ComPtr<IUnknown> rightIdentity;
			return SUCCEEDED(a_left->QueryInterface(IID_PPV_ARGS(leftIdentity.GetAddressOf()))) &&
			       SUCCEEDED(a_right->QueryInterface(IID_PPV_ARGS(rightIdentity.GetAddressOf()))) &&
			       leftIdentity.Get() == rightIdentity.Get();
		}

		void ResetInitialization(RendererState& a_state)
		{
			if (Win32Platform::HasLiveBackend()) {
				logger::critical(
					"Refusing to destroy an ImGui context while the Win32 backend is live");
				return;
			}
			rendererReady.store(false, std::memory_order_release);

			if (a_state.Context) {
				ImGui::SetCurrentContext(a_state.Context);
				if (ImGui::GetIO().BackendRendererUserData) {
					ImGui_ImplDX12_Shutdown();
				}
				ImGui::DestroyContext(a_state.Context);
				a_state.Context = nullptr;
				a_state.ContextGeneration = 0;
			}

			a_state.RenderTargetHeap.Reset();
			a_state.ShaderHeap.Reset();
			a_state.CompletionBuffer.Reset();
			a_state.Device.Reset();
		}

		[[nodiscard]] bool ReadCompletionValue(
			RendererState& a_state,
			std::size_t    a_slot,
			std::uint32_t& a_value)
		{
			const auto        byteOffset = a_slot * sizeof(std::uint32_t);
			const D3D12_RANGE readRange{ byteOffset, byteOffset + sizeof(std::uint32_t) };
			void*             mappedData{};
			if (FAILED(a_state.CompletionBuffer->Map(0, &readRange, &mappedData)) || !mappedData) {
				return false;
			}

			a_value = static_cast<const volatile std::uint32_t*>(mappedData)[a_slot];
			constexpr D3D12_RANGE noCpuWrites{ 0, 0 };
			a_state.CompletionBuffer->Unmap(0, &noCpuWrites);
			return true;
		}

		[[nodiscard]] bool InitializeCompletionBuffer(RendererState& a_state)
		{
			D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
			if (FAILED(a_state.Device->CheckFeatureSupport(
					D3D12_FEATURE_D3D12_OPTIONS3,
					&options,
					sizeof(options))) ||
				(options.WriteBufferImmediateSupportFlags &
					D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT) == 0) {
				logger::critical("Direct command lists do not support GPU completion markers");
				return false;
			}

			D3D12_HEAP_PROPERTIES heapProperties{};
			heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
			heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
			heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
			heapProperties.CreationNodeMask = 1;
			heapProperties.VisibleNodeMask = 1;

			D3D12_RESOURCE_DESC bufferDescription{};
			bufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufferDescription.Width = frameResourceCount * sizeof(std::uint32_t);
			bufferDescription.Height = 1;
			bufferDescription.DepthOrArraySize = 1;
			bufferDescription.MipLevels = 1;
			bufferDescription.Format = DXGI_FORMAT_UNKNOWN;
			bufferDescription.SampleDesc.Count = 1;
			bufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			bufferDescription.Flags = D3D12_RESOURCE_FLAG_NONE;

			if (FAILED(a_state.Device->CreateCommittedResource(
					&heapProperties,
					D3D12_HEAP_FLAG_NONE,
					&bufferDescription,
					D3D12_RESOURCE_STATE_COPY_DEST,
					nullptr,
					IID_PPV_ARGS(a_state.CompletionBuffer.GetAddressOf())))) {
				logger::critical("Failed to create the GPU completion-marker buffer");
				return false;
			}

			for (std::size_t slot = 0; slot < frameResourceCount; ++slot) {
				if (!ReadCompletionValue(
						a_state,
						slot,
						a_state.CompletionSlots[slot].LastCompletedValue)) {
					logger::critical("Failed to map the GPU completion-marker buffer");
					return false;
				}
			}

			return true;
		}

		[[nodiscard]] bool AcquireFrameSlot(RendererState& a_state, std::size_t& a_slot)
		{
			a_slot = static_cast<std::size_t>(a_state.NextFrameIndex % frameResourceCount);
			auto& slot = a_state.CompletionSlots[a_slot];
			if (!slot.Pending) {
				return true;
			}

			std::uint32_t completedValue{};
			if (!ReadCompletionValue(a_state, a_slot, completedValue) ||
				completedValue != slot.PendingValue) {
				return false;
			}

			slot.LastCompletedValue = completedValue;
			slot.Pending = false;
			return true;
		}

		void MarkFrameSlot(
			RendererState&              a_state,
			ID3D12GraphicsCommandList2* a_commandList,
			std::size_t                 a_slot)
		{
			auto& slot = a_state.CompletionSlots[a_slot];
			slot.PendingValue = slot.LastCompletedValue + 1;

			D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker{};
			marker.Dest = a_state.CompletionBuffer->GetGPUVirtualAddress() +
			              a_slot * sizeof(std::uint32_t);
			marker.Value = slot.PendingValue;
			constexpr D3D12_WRITEBUFFERIMMEDIATE_MODE mode =
				D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
			a_commandList->WriteBufferImmediate(1, &marker, &mode);

			slot.Pending = true;
			++a_state.NextFrameIndex;
		}

		[[nodiscard]] bool InitializeLocked(RendererState& a_state, ID3D12Device* a_device)
		{
			if (a_state.Context) {
				return a_state.Device.Get() == a_device;
			}

			if (a_state.InitializationFailed) {
				return false;
			}

			a_state.Device = a_device;
			if (!InitializeCompletionBuffer(a_state)) {
				a_state.InitializationFailed = true;
				ResetInitialization(a_state);
				return false;
			}

			D3D12_DESCRIPTOR_HEAP_DESC shaderHeapDescription{};
			shaderHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			shaderHeapDescription.NumDescriptors = 1;
			shaderHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

			if (FAILED(a_device->CreateDescriptorHeap(
					&shaderHeapDescription,
					IID_PPV_ARGS(a_state.ShaderHeap.GetAddressOf())))) {
				logger::critical("Failed to create the ImGui shader descriptor heap");
				a_state.InitializationFailed = true;
				ResetInitialization(a_state);
				return false;
			}

			D3D12_DESCRIPTOR_HEAP_DESC renderTargetHeapDescription{};
			renderTargetHeapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			renderTargetHeapDescription.NumDescriptors = 1;
			renderTargetHeapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

			if (FAILED(a_device->CreateDescriptorHeap(
					&renderTargetHeapDescription,
					IID_PPV_ARGS(a_state.RenderTargetHeap.GetAddressOf())))) {
				logger::critical("Failed to create the ImGui render-target descriptor heap");
				a_state.InitializationFailed = true;
				ResetInitialization(a_state);
				return false;
			}

			IMGUI_CHECKVERSION();
			a_state.Context = ImGui::CreateContext();
			if (!a_state.Context) {
				logger::critical("Failed to create the ImGui context");
				a_state.InitializationFailed = true;
				ResetInitialization(a_state);
				return false;
			}
			a_state.ContextGeneration =
				nextContextGeneration.fetch_add(1, std::memory_order_relaxed);

			ImGui::SetCurrentContext(a_state.Context);
			auto& io = ImGui::GetIO();
			io.IniFilename = imguiIniFilename;
			io.LogFilename = nullptr;
			io.ConfigWindowsResizeFromEdges = true;
			io.ConfigWindowsMoveFromTitleBarOnly = true;
			io.ConfigFlags |=
				ImGuiConfigFlags_NavEnableKeyboard |
				ImGuiConfigFlags_NavEnableGamepad |
				ImGuiConfigFlags_NoMouseCursorChange;
			ImFontConfig fontConfiguration{};
			fontConfiguration.SizePixels = defaultFontSizePixels;
			io.FontDefault = io.Fonts->AddFontDefault(&fontConfiguration);
			if (!io.FontDefault || !io.Fonts->Build()) {
				logger::critical("Failed to build the 32 px ImGui font atlas");
				a_state.InitializationFailed = true;
				ResetInitialization(a_state);
				return false;
			}
			ImGui::StyleColorsDark();

			const auto shaderCpuHandle = a_state.ShaderHeap->GetCPUDescriptorHandleForHeapStart();
			const auto shaderGpuHandle = a_state.ShaderHeap->GetGPUDescriptorHandleForHeapStart();
			if (!ImGui_ImplDX12_Init(
					a_device,
					static_cast<int>(frameResourceCount),
					DXGI_FORMAT_R8G8B8A8_UNORM,
					a_state.ShaderHeap.Get(),
					shaderCpuHandle,
					shaderGpuHandle)) {
				logger::critical("Failed to initialize the ImGui DirectX 12 backend");
				a_state.InitializationFailed = true;
				ResetInitialization(a_state);
				return false;
			}

			if (!ImGui_ImplDX12_CreateDeviceObjects()) {
				logger::critical("Failed to create the ImGui DirectX 12 device objects");
				a_state.InitializationFailed = true;
				ResetInitialization(a_state);
				return false;
			}

			rendererReady.store(true, std::memory_order_release);
			logger::info("ImGui DirectX 12 renderer initialized");
			return true;
		}

		[[nodiscard]] bool HasValidHeapSnapshot(const DescriptorHeapSnapshot& a_snapshot)
		{
			if (a_snapshot.Count == 0 || a_snapshot.Count > a_snapshot.Heaps.size()) {
				return false;
			}

			for (UINT index = 0; index < a_snapshot.Count; ++index) {
				if (!a_snapshot.Heaps[index]) {
					return false;
				}
			}

			return true;
		}
	}

	bool Initialize(ID3D12Device* a_device)
	{
		if (!a_device) {
			return false;
		}

		std::scoped_lock lock{ GetRendererMutex() };
		return InitializeLocked(GetRendererState(), a_device);
	}

	void SetPlatformInputEnabled(bool a_enabled)
	{
		Win32Platform::UpdateInputState(
			a_enabled && rendererReady.load(std::memory_order_acquire) &&
			Win32Platform::IsInitialized() && Win32Platform::IsHostWindowUsable());
	}

	bool HasRecentMainWindowFrame(std::uint64_t a_generation) noexcept
	{
		if (a_generation == 0 ||
			renderedMainWindowGeneration.load(std::memory_order_acquire) != a_generation) {
			return false;
		}

		const auto lastTick = lastMainWindowRenderTick.load(std::memory_order_acquire);
		return lastTick != 0 &&
		       (::GetTickCount64() - lastTick) <= maximumMainWindowFrameAgeMilliseconds;
	}

	RenderResult Render(
		ID3D12GraphicsCommandList*    a_commandList,
		ID3D12Resource*               a_renderTarget,
		const DescriptorHeapSnapshot& a_engineHeaps,
		SetDescriptorHeapsFunction    a_setDescriptorHeaps)
	{
		if (!a_commandList || !a_renderTarget || !a_setDescriptorHeaps ||
			a_commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
			!HasValidHeapSnapshot(a_engineHeaps)) {
			return RenderResult::InvalidArguments;
		}

		std::scoped_lock lock{ GetRendererMutex() };

		Microsoft::WRL::ComPtr<ID3D12Device> commandListDevice;
		if (FAILED(a_commandList->GetDevice(IID_PPV_ARGS(commandListDevice.GetAddressOf())))) {
			return RenderResult::DeviceQueryFailed;
		}

		auto& rendererState = GetRendererState();
		if (!rendererState.Context ||
			!HasSameComIdentity(rendererState.Device.Get(), commandListDevice.Get())) {
			return RenderResult::DeviceMismatch;
		}

		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList2;
		if (FAILED(a_commandList->QueryInterface(IID_PPV_ARGS(commandList2.GetAddressOf())))) {
			return RenderResult::CommandList2Unavailable;
		}

		std::size_t frameSlot{};
		if (!AcquireFrameSlot(rendererState, frameSlot)) {
			return RenderResult::FrameSlotBusy;
		}

		const auto description = a_renderTarget->GetDesc();
		if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
			description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS ||
			description.SampleDesc.Count != 1 || description.Width < 256 ||
			description.Height < 256) {
			return RenderResult::InvalidTarget;
		}

		ImGui::SetCurrentContext(rendererState.Context);
		if (!Win32Platform::PrepareFrame()) {
			return RenderResult::PlatformFrameUnavailable;
		}
		ImGui_ImplDX12_NewFrame();
		auto& io = ImGui::GetIO();
		if (!std::isfinite(io.DisplaySize.x) ||
			!std::isfinite(io.DisplaySize.y) ||
			io.DisplaySize.x <= 0.0F ||
			io.DisplaySize.y <= 0.0F) {
			return RenderResult::InvalidDisplaySize;
		}

		io.DisplayFramebufferScale = ImVec2{
			static_cast<float>(description.Width) / io.DisplaySize.x,
			static_cast<float>(description.Height) / io.DisplaySize.y
		};
		const auto openGeneration = WindowManager::GetMainWindowOpenGeneration();
		if (!HasRecentMainWindowFrame(openGeneration) ||
			!WindowManager::IsMainWindowOpenGeneration(openGeneration)) {
			io.ClearEventsQueue();
			io.ClearInputKeys();
		}
		ImGui::NewFrame();
		ImGuiMemAllocFunc allocate{};
		ImGuiMemFreeFunc free{};
		void* allocatorUserData{};
		ImGui::GetAllocatorFunctions(&allocate, &free, &allocatorUserData);
		static_assert(std::is_same_v<Model::ImGuiAllocateFunction, ImGuiMemAllocFunc>);
		static_assert(std::is_same_v<Model::ImGuiFreeFunction, ImGuiMemFreeFunc>);
		const Model::RenderContext renderContext{
			.StructureSize = sizeof(Model::RenderContext),
			.InterfaceVersion = Model::INTERFACE_VERSION,
			.ContextGeneration = rendererState.ContextGeneration,
			.ImGuiContext = rendererState.Context,
			.Allocate = allocate,
			.Free = free,
			.AllocatorUserData = allocatorUserData
		};
		const auto renderedGeneration =
			WindowManager::RenderOpenWindows(renderContext);
		ImGui::Render();

		D3D12_RENDER_TARGET_VIEW_DESC renderTargetView{};
		renderTargetView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		renderTargetView.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		renderTargetView.Texture2D.MipSlice = 0;
		renderTargetView.Texture2D.PlaneSlice = 0;

		const auto renderTargetHandle =
			rendererState.RenderTargetHeap->GetCPUDescriptorHandleForHeapStart();
		rendererState.Device->CreateRenderTargetView(
			a_renderTarget,
			&renderTargetView,
			renderTargetHandle);

		ID3D12DescriptorHeap* frameworkHeaps[]{ rendererState.ShaderHeap.Get() };
		a_setDescriptorHeaps(a_commandList, 1, frameworkHeaps);
		a_commandList->OMSetRenderTargets(1, &renderTargetHandle, FALSE, nullptr);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), a_commandList);
		MarkFrameSlot(rendererState, commandList2.Get(), frameSlot);

		a_setDescriptorHeaps(
			a_commandList,
			a_engineHeaps.Count,
			a_engineHeaps.Heaps.data());

		if (renderedGeneration != 0) {
			renderedMainWindowGeneration.store(
				renderedGeneration,
				std::memory_order_release);
			lastMainWindowRenderTick.store(::GetTickCount64(), std::memory_order_release);
			static_cast<void>(Win32Platform::PostHostWindowCallback());
		}

		return RenderResult::Rendered;
	}
}
