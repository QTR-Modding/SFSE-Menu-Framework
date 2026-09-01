#include "D3D12Renderer.h"

#include "Appearance.h"
#include "FrameworkRuntime.h"
#include "Win32Platform.h"

#include <backends/imgui_impl_dx12.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <utility>

#include <wrl/client.h>

namespace SFSEMenuFramework::D3D12Renderer
{
	bool HasSameDeviceIdentity(
		ID3D12Device* a_left, ID3D12Device* a_right) noexcept
	{
		if (!a_left || !a_right) {
			return false;
		}

		Microsoft::WRL::ComPtr<IUnknown> leftIdentity;
		Microsoft::WRL::ComPtr<IUnknown> rightIdentity;
		return SUCCEEDED(a_left->QueryInterface(IID_PPV_ARGS(leftIdentity.GetAddressOf()))) &&
		       SUCCEEDED(a_right->QueryInterface(IID_PPV_ARGS(rightIdentity.GetAddressOf()))) &&
		       leftIdentity.Get() == rightIdentity.Get();
	}

	namespace
	{
		using Microsoft::WRL::ComPtr;
		constexpr std::size_t frameResourceCount = 4;
		constexpr std::uint64_t maximumBlockingWindowFrameAgeMilliseconds = 250;
		constexpr char imguiIniFilename[] =
			"Data/SFSE/Plugins/SFSEMenuFramework.imgui.ini";

		std::atomic<std::uint64_t> renderedBlockingWindowGeneration{ 0 };
		std::atomic<std::uint64_t> lastBlockingWindowRenderTick{ 0 };
		std::atomic<std::uint64_t> nextContextGeneration{ 1 };
		std::atomic<bool>          rendererReady{ false };
		thread_local bool          renderInProgress{};

		struct RenderScope final
		{
			bool& InProgress;

			~RenderScope() noexcept
			{
				InProgress = false;
			}
		};

		struct FontResources final
		{
			ComPtr<ID3D12DescriptorHeap> ShaderHeap;
			ComPtr<ID3D12Resource>       Texture;
			ComPtr<ID3D12Resource>       UploadBuffer;

			void Reset() noexcept
			{
				UploadBuffer.Reset();
				Texture.Reset();
				ShaderHeap.Reset();
			}
		};

		struct CompletionSlot final
		{
			std::uint32_t LastCompletedValue{ 0 };
			std::uint32_t PendingValue{ 0 };
			FontResources Resources;
		};

		struct RendererState final
		{
			ComPtr<ID3D12Device>                           Device;
			ComPtr<ID3D12DescriptorHeap>                   ShaderHeap;
			ComPtr<ID3D12DescriptorHeap>                   RenderTargetHeap;
			ComPtr<ID3D12Resource>                         CompletionBuffer;
			std::array<CompletionSlot, frameResourceCount> CompletionSlots{};
			FontResources                                  ActiveFontResources;
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

		[[nodiscard]] bool CheckResult(HRESULT a_result, const char* a_operation) noexcept
		{
			if (SUCCEEDED(a_result)) {
				return true;
			}
			logger::error(
				"D3D12 {} failed with HRESULT 0x{:08X}",
				a_operation,
				static_cast<std::uint32_t>(a_result));
			return false;
		}

		[[nodiscard]] D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE a_type) noexcept
		{
			D3D12_HEAP_PROPERTIES result{};
			result.Type = a_type;
			result.CreationNodeMask = 1;
			result.VisibleNodeMask = 1;
			return result;
		}

		[[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(std::uint64_t a_size) noexcept
		{
			D3D12_RESOURCE_DESC result{};
			result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			result.Width = a_size;
			result.Height = 1;
			result.DepthOrArraySize = 1;
			result.MipLevels = 1;
			result.SampleDesc.Count = 1;
			result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			return result;
		}

		[[nodiscard]] bool CreateDescriptorHeap(
			ID3D12Device*                  a_device,
			D3D12_DESCRIPTOR_HEAP_TYPE     a_type,
			D3D12_DESCRIPTOR_HEAP_FLAGS    a_flags,
			ComPtr<ID3D12DescriptorHeap>& a_result) noexcept
		{
			D3D12_DESCRIPTOR_HEAP_DESC description{};
			description.Type = a_type;
			description.NumDescriptors = 1;
			description.Flags = a_flags;
			return CheckResult(
				a_device->CreateDescriptorHeap(
					&description,
					IID_PPV_ARGS(a_result.GetAddressOf())),
				"descriptor-heap creation");
		}

		// Adapted from Dear ImGui 1.90.8 imgui_impl_dx12.cpp at
		// 6f7b5d0ee2fe9948ab871a530888a6dc5c960700 (MIT). Each live atlas gets
		// its own heap; CompletionSlot keeps its resources alive through GPU use.
		[[nodiscard]] bool CreateFontTexture(
			ID3D12Device*              a_device,
			ID3D12GraphicsCommandList* a_commandList,
			const unsigned char*       a_pixels,
			int                        a_width,
			int                        a_height,
			FontResources&             a_result) noexcept
		{
			a_result.Reset();
			if (!a_device || !a_commandList ||
				a_commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT || !a_pixels ||
				a_width <= 0 || a_height <= 0 ||
				a_width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
				a_height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
				logger::error("Rejected invalid live font texture {}x{}", a_width, a_height);
				return false;
			}

			const auto rowBytes = static_cast<std::uint64_t>(a_width) * 4;
			const auto uploadPitch =
				(rowBytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
				~static_cast<std::uint64_t>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
			const auto uploadSize = uploadPitch * static_cast<std::uint64_t>(a_height);
			FontResources candidate;
			if (!CreateDescriptorHeap(
					a_device,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
					D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
					candidate.ShaderHeap) ||
				candidate.ShaderHeap->GetGPUDescriptorHandleForHeapStart().ptr == 0) {
				return false;
			}

			D3D12_RESOURCE_DESC texture{};
			texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			texture.Width = static_cast<UINT64>(a_width);
			texture.Height = static_cast<UINT>(a_height);
			texture.DepthOrArraySize = 1;
			texture.MipLevels = 1;
			texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			texture.SampleDesc.Count = 1;
			texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			const auto defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
			if (!CheckResult(
					a_device->CreateCommittedResource(
						&defaultHeap,
						D3D12_HEAP_FLAG_NONE,
						&texture,
						D3D12_RESOURCE_STATE_COPY_DEST,
						nullptr,
						IID_PPV_ARGS(candidate.Texture.GetAddressOf())),
					"font-texture creation")) {
				return false;
			}

			const auto uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
			const auto upload = BufferDescription(uploadSize);
			if (!CheckResult(
					a_device->CreateCommittedResource(
						&uploadHeap,
						D3D12_HEAP_FLAG_NONE,
						&upload,
						D3D12_RESOURCE_STATE_GENERIC_READ,
						nullptr,
						IID_PPV_ARGS(candidate.UploadBuffer.GetAddressOf())),
					"font-upload creation")) {
				return false;
			}

			void* mapped{};
			constexpr D3D12_RANGE noCpuReads{ 0, 0 };
			if (!CheckResult(candidate.UploadBuffer->Map(0, &noCpuReads, &mapped), "font-upload map") ||
				!mapped) {
				return false;
			}
			for (int row = 0; row < a_height; ++row) {
				std::memcpy(
					static_cast<std::byte*>(mapped) +
						static_cast<std::size_t>(row) * static_cast<std::size_t>(uploadPitch),
					a_pixels + static_cast<std::size_t>(row) * static_cast<std::size_t>(rowBytes),
					static_cast<std::size_t>(rowBytes));
			}
			const D3D12_RANGE cpuWrites{ 0, static_cast<SIZE_T>(uploadSize) };
			candidate.UploadBuffer->Unmap(0, &cpuWrites);

			D3D12_TEXTURE_COPY_LOCATION source{};
			source.pResource = candidate.UploadBuffer.Get();
			source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			source.PlacedFootprint.Footprint.Width = static_cast<UINT>(a_width);
			source.PlacedFootprint.Footprint.Height = static_cast<UINT>(a_height);
			source.PlacedFootprint.Footprint.Depth = 1;
			source.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(uploadPitch);
			D3D12_TEXTURE_COPY_LOCATION destination{};
			destination.pResource = candidate.Texture.Get();
			destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			a_commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = candidate.Texture.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			a_commandList->ResourceBarrier(1, &barrier);

			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			a_device->CreateShaderResourceView(
				candidate.Texture.Get(),
				&view,
				candidate.ShaderHeap->GetCPUDescriptorHandleForHeapStart());
			a_result = std::move(candidate);
			return true;
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
			a_state.ActiveFontResources.Reset();
			for (auto& slot : a_state.CompletionSlots) {
				slot.Resources.Reset();
				slot.PendingValue = 0;
			}
			a_state.ShaderHeap.Reset();
			a_state.CompletionBuffer.Reset();
			a_state.Device.Reset();
		}

		[[nodiscard]] bool ReadCompletionValue(
			RendererState& a_state,
			std::size_t    a_slot,
			std::uint32_t& a_value)
		{
			const auto byteOffset = a_slot * sizeof(std::uint32_t);
			const D3D12_RANGE readRange{ byteOffset, byteOffset + sizeof(std::uint32_t) };
			void* mappedData{};
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

			const auto heapProperties = HeapProperties(D3D12_HEAP_TYPE_READBACK);
			const auto bufferDescription =
				BufferDescription(frameResourceCount * sizeof(std::uint32_t));
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
			if (!slot.PendingValue) {
				return true;
			}
			std::uint32_t completedValue{};
			if (!ReadCompletionValue(a_state, a_slot, completedValue) ||
				completedValue != slot.PendingValue) {
				return false;
			}
			slot.LastCompletedValue = completedValue;
			slot.Resources.Reset();
			slot.PendingValue = 0;
			return true;
		}

		void MarkFrameSlot(
			RendererState&              a_state,
			ID3D12GraphicsCommandList2* a_commandList,
			std::size_t                 a_slot)
		{
			auto& slot = a_state.CompletionSlots[a_slot];
			slot.PendingValue = slot.LastCompletedValue + 1;
			if (!slot.PendingValue) {
				slot.PendingValue = 1;
			}

			D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker{};
			marker.Dest = a_state.CompletionBuffer->GetGPUVirtualAddress() +
			              a_slot * sizeof(std::uint32_t);
			marker.Value = slot.PendingValue;
			constexpr D3D12_WRITEBUFFERIMMEDIATE_MODE mode =
				D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
			a_commandList->WriteBufferImmediate(1, &marker, &mode);

			slot.Resources = a_state.ActiveFontResources;
			a_state.ActiveFontResources.UploadBuffer.Reset();
			++a_state.NextFrameIndex;
		}

		struct FontUploadContext final
		{
			ID3D12Device*              Device{};
			ID3D12GraphicsCommandList* CommandList{};
			FontResources              Candidate;
		};

		[[nodiscard]] bool BuildFontTexture(
			const unsigned char*             a_pixels,
			int                              a_width,
			int                              a_height,
			FontManager::TextureBuildResult& a_result,
			void*                            a_userData) noexcept
		{
			auto* context = static_cast<FontUploadContext*>(a_userData);
			if (!context || !CreateFontTexture(
					context->Device,
					context->CommandList,
					a_pixels,
					a_width,
					a_height,
					context->Candidate)) {
				return false;
			}

			static_assert(sizeof(std::uintptr_t) >= sizeof(UINT64));
			a_result.TextureID = static_cast<std::uintptr_t>(
				context->Candidate.ShaderHeap
					->GetGPUDescriptorHandleForHeapStart()
					.ptr);
			return true;
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
			auto fail = [&a_state]() {
				a_state.InitializationFailed = true;
				ResetInitialization(a_state);
				return false;
			};
			if (!InitializeCompletionBuffer(a_state)) {
				return fail();
			}

			if (!CreateDescriptorHeap(
					a_device,
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
					D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
					a_state.ShaderHeap)) {
				return fail();
			}
			if (!CreateDescriptorHeap(
					a_device,
					D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
					D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
					a_state.RenderTargetHeap)) {
				return fail();
			}

			IMGUI_CHECKVERSION();
			a_state.Context = ImGui::CreateContext();
			if (!a_state.Context) {
				logger::critical("Failed to create the ImGui context");
				return fail();
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
			if (!FontManager::BuildDefaultAtlas(io)) {
				logger::critical("Failed to build the configured ImGui font atlas");
				return fail();
			}
			ThemeManager::Initialize();

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
				return fail();
			}

			if (!ImGui_ImplDX12_CreateDeviceObjects()) {
				logger::critical("Failed to create the ImGui DirectX 12 device objects");
				return fail();
			}
			a_state.ActiveFontResources.Reset();
			a_state.ActiveFontResources.ShaderHeap = a_state.ShaderHeap;

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

	bool SetPlatformInputEnabled(
		bool a_enabled,
		std::uint64_t a_earlyRawMouseGeneration)
	{
		const bool canEnable =
			rendererReady.load(std::memory_order_acquire) &&
			Win32Platform::IsInitialized() && Win32Platform::IsHostWindowUsable();
		const bool enable = a_enabled && canEnable;
		const bool applied = Win32Platform::UpdateInputState(
			enable,
			enable ? a_earlyRawMouseGeneration : 0);
		return a_enabled ? enable && applied : applied;
	}

	bool HasRecentBlockingWindowFrame(std::uint64_t a_generation) noexcept
	{
		if (a_generation == 0 ||
			renderedBlockingWindowGeneration.load(std::memory_order_acquire) != a_generation) {
			return false;
		}

		const auto lastTick = lastBlockingWindowRenderTick.load(std::memory_order_acquire);
		return lastTick != 0 &&
		       (::GetTickCount64() - lastTick) <= maximumBlockingWindowFrameAgeMilliseconds;
	}

	void Render(
		ID3D12GraphicsCommandList*    a_commandList,
		ID3D12Resource*               a_renderTarget,
		const DescriptorHeapSnapshot& a_engineHeaps,
		SetDescriptorHeapsFunction    a_setDescriptorHeaps)
	{
		if (!a_commandList || !a_renderTarget || !a_setDescriptorHeaps ||
			a_commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
			!HasValidHeapSnapshot(a_engineHeaps)) {
			return;
		}

		std::scoped_lock lock{ GetRendererMutex() };
		if (renderInProgress) {
			return;
		}
		renderInProgress = true;
		const RenderScope renderScope{ renderInProgress };

		Microsoft::WRL::ComPtr<ID3D12Device> commandListDevice;
		if (FAILED(a_commandList->GetDevice(IID_PPV_ARGS(commandListDevice.GetAddressOf())))) {
			return;
		}

		auto& rendererState = GetRendererState();
		if (!rendererState.Context ||
			!HasSameDeviceIdentity(rendererState.Device.Get(), commandListDevice.Get())) {
			return;
		}

		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList2;
		if (FAILED(a_commandList->QueryInterface(IID_PPV_ARGS(commandList2.GetAddressOf())))) {
			return;
		}

		std::size_t frameSlot{};
		if (!AcquireFrameSlot(rendererState, frameSlot)) {
			return;
		}

		const auto description = a_renderTarget->GetDesc();
		if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
			description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS ||
			description.SampleDesc.Count != 1 || description.Width < 256 ||
			description.Height < 256) {
			return;
		}

		EventManager::Snapshot lifecycleSnapshot;
		if (!EventManager::BeginFrame(lifecycleSnapshot)) {
			return;
		}

		ImGui::SetCurrentContext(rendererState.Context);
		auto& io = ImGui::GetIO();
		if (!Win32Platform::PrepareFrame()) {
			return;
		}
		ImGui_ImplDX12_NewFrame();
		if (!std::isfinite(io.DisplaySize.x) ||
			!std::isfinite(io.DisplaySize.y) ||
			io.DisplaySize.x <= 0.0F ||
			io.DisplaySize.y <= 0.0F) {
			return;
		}

		FontUploadContext fontUpload{
			.Device = rendererState.Device.Get(),
			.CommandList = a_commandList
		};
		const auto fontApplyResult = FontManager::ApplyPendingAtlas(
			io,
			BuildFontTexture,
			&fontUpload);
		if (fontApplyResult == FontManager::LiveApplyResult::Applied) {
			rendererState.ActiveFontResources = std::move(fontUpload.Candidate);
			if (!ThemeManager::QueueUIScale(FontManager::GetActiveUIScale())) {
				logger::critical("Could not queue the validated live UI scale");
			}
			logger::info("Applied live ImGui font and UI scale");
		}
		ThemeManager::ApplyPending();

		io.DisplayFramebufferScale = ImVec2{
			static_cast<float>(description.Width) / io.DisplaySize.x,
			static_cast<float>(description.Height) / io.DisplaySize.y
		};
		const auto openGeneration = WindowManager::GetBlockingWindowOpenGeneration();
		if (!HasRecentBlockingWindowFrame(openGeneration) ||
			!WindowManager::IsBlockingWindowOpenGeneration(openGeneration)) {
			io.ClearEventsQueue();
			io.ClearInputKeys();
		}
		EventManager::Dispatch(
			Model::EventType::kBeforeRender,
			lifecycleSnapshot);
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

		auto* activeFontHeap =
			rendererState.ActiveFontResources.ShaderHeap.Get();
		if (!activeFontHeap) {
			return;
		}
		ID3D12DescriptorHeap* frameworkHeaps[]{ activeFontHeap };
		a_setDescriptorHeaps(a_commandList, 1, frameworkHeaps);
		a_commandList->OMSetRenderTargets(1, &renderTargetHandle, FALSE, nullptr);
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), a_commandList);
		MarkFrameSlot(rendererState, commandList2.Get(), frameSlot);

		a_setDescriptorHeaps(
			a_commandList,
			a_engineHeaps.Count,
			a_engineHeaps.Heaps.data());

		if (renderedGeneration != 0) {
			renderedBlockingWindowGeneration.store(
				renderedGeneration,
				std::memory_order_release);
			lastBlockingWindowRenderTick.store(::GetTickCount64(), std::memory_order_release);
			static_cast<void>(Win32Platform::PostHostWindowCallback());
		}

		EventManager::Dispatch(
			Model::EventType::kAfterRender,
			lifecycleSnapshot);

	}
}
