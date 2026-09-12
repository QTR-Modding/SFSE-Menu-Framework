#include "rendering/D3D12Renderer.h"
#include "audio/MenuSounds.h"
#include "rendering/D3D12Texture.h"

#include "appearance/FontManager.h"
#include "appearance/CursorManager.h"
#include "appearance/ThemeManager.h"
#include "input/GamepadNavigation.h"
#include "config/FrameworkSettings.h"
#include "input/InputEventManager.h"
#include "platform/win32/Win32Platform.h"
#include "runtime/EventManager.h"
#include "runtime/HudManager.h"
#include "runtime/WindowManager.h"
#include "ui/SettingsWindow.h"
#include "ui/WindowPlacement.h"

#include <backends/imgui_impl_dx12.h>
#include "DX12Draw.h"
#include <imgui.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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
		using D3D12Textures::BufferDescription;
		using D3D12Textures::CreateDescriptorHeap;
		using D3D12Textures::HeapProperties;
		constexpr std::size_t frameResourceCount = 4;
		constexpr std::size_t imageCount = 2;  // Wallpaper and cursor; font is descriptor 0.
		constexpr std::uint64_t maximumBlockingWindowFrameAgeMilliseconds = 250;
		constexpr char imguiIniFilename[] =
			"Data/SFSE/Plugins/SFSEMenuFramework.imgui.ini";

		std::atomic<std::uint64_t> renderedBlockingWindowGeneration{ 0 };
		std::atomic<std::uint64_t> lastBlockingWindowRenderTick{ 0 };
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

		struct CompletionSlot final
		{
			std::uint32_t LastCompletedValue{ 0 };
			std::uint32_t PendingValue{ 0 };
			D3D12Textures::Texture Resources;
			std::array<D3D12Textures::Texture, imageCount> Images;
			ComPtr<ID3D12DescriptorHeap> TextureHeap;
		};

		struct RendererState final
		{
			ComPtr<ID3D12Device>                           Device;
			ComPtr<ID3D12DescriptorHeap>                   ShaderHeap;
			ComPtr<ID3D12DescriptorHeap>                   RenderTargetHeap;
			ComPtr<ID3D12Resource>                         CompletionBuffer;
			std::array<CompletionSlot, frameResourceCount> CompletionSlots{};
			D3D12Textures::Texture                        ActiveFontResources;
			std::array<D3D12Textures::Texture, imageCount> ActiveImages;
			std::array<std::shared_ptr<const ThemeImage>, imageCount> Images;
			std::uint64_t                                  NextFrameIndex{ 0 };
			ImGuiContext*                                  Context{ nullptr };
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

		void ResetInitialization(RendererState& a_state)
		{
			InputEventManager::SetImGuiItemActive(false);
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
			}

			a_state.RenderTargetHeap.Reset();
			a_state.ActiveFontResources.Reset();
			a_state.ActiveImages = {};
			a_state.Images = {};
			ThemeManager::SetWallpaperTexture(0, false);
			CursorManager::SetTexture(0, false);
			for (auto& slot : a_state.CompletionSlots) {
				slot.Resources.Reset();
				slot.Images = {};
				slot.TextureHeap.Reset();
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
				if (!CreateDescriptorHeap(a_state.Device.Get(),
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
					D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
					a_state.CompletionSlots[slot].TextureHeap, static_cast<UINT>(1 + imageCount))) {
					return false;
				}
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

		[[nodiscard]] bool SharedUploadsComplete(RendererState& a_state)
		{
			// Recording can run ahead of Present. A texture published by that recording
			// must finish uploading before another path can use it, even on the same queue.
			for (std::size_t i = 0; i < frameResourceCount; ++i) {
				auto& slot = a_state.CompletionSlots[i];
				bool hasUploads = slot.Resources.UploadBuffer != nullptr;
				for (const auto& image : slot.Images) {
					hasUploads |= image.UploadBuffer != nullptr;
				}
				if (!hasUploads) { continue; }
				std::uint32_t completed{};
				if (!ReadCompletionValue(a_state, i, completed) || completed != slot.PendingValue) {
					return false;
				}
				slot.Resources.UploadBuffer.Reset();
				for (auto& image : slot.Images) { image.UploadBuffer.Reset(); }
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
			slot.Images = {};
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
			slot.Images = a_state.ActiveImages;
			a_state.ActiveFontResources.UploadBuffer.Reset();
			for (auto& image : a_state.ActiveImages) {
				image.UploadBuffer.Reset();
			}
			++a_state.NextFrameIndex;
		}

		struct FontUploadContext final
		{
			ID3D12Device*              Device{};
			ID3D12GraphicsCommandList* CommandList{};
			D3D12Textures::Texture    Candidate;
		};

		[[nodiscard]] bool BuildFontTexture(
			const unsigned char*             a_pixels,
			int                              a_width,
			int                              a_height,
			FontManager::TextureBuildResult& a_result,
			void*                            a_userData) noexcept
		{
			auto* context = static_cast<FontUploadContext*>(a_userData);
			if (!context || !D3D12Textures::Upload(
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
			static_cast<void>(ThemeManager::QueueUIScale(FontManager::GetActiveUIScale()));
			ThemeManager::ApplyPending();

			// Keep a CPU-only source descriptor for safe copies into frame heaps.
			if (!CreateDescriptorHeap(a_device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				D3D12_DESCRIPTOR_HEAP_FLAG_NONE, a_state.ActiveFontResources.ViewHeap)) {
				return fail();
			}
			const auto shaderCpuHandle =
				a_state.ActiveFontResources.ViewHeap->GetCPUDescriptorHandleForHeapStart();
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
			a_device->CopyDescriptorsSimple(1,
				a_state.ShaderHeap->GetCPUDescriptorHandleForHeapStart(),
				shaderCpuHandle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			a_state.ActiveFontResources.ShaderHeap = a_state.ShaderHeap;

			rendererReady.store(true, std::memory_order_release);
			logger::info("ImGui DirectX 12 renderer initialized");
			return true;
		}

		[[nodiscard]] bool PrepareThemeImages(RendererState& a_state,
			ID3D12GraphicsCommandList* a_commandList, std::size_t a_slot)
		{
			std::array<std::shared_ptr<const ThemeImage>, imageCount> images{
				ThemeManager::GetWallpaperImage(), CursorManager::GetImage()
			};
			const auto step = a_state.Device->GetDescriptorHandleIncrementSize(
				D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto* heap = a_state.CompletionSlots[a_slot].TextureHeap.Get();
			auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
			auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
			bool hasImages = false;
			for (std::size_t i = 0; i < images.size(); ++i) {
				const auto& image = images[i];
				auto& texture = a_state.ActiveImages[i];
				if (image != a_state.Images[i]) {
					a_state.Images[i] = image;
					texture.Reset();
					if (image && !D3D12Textures::Upload(a_state.Device.Get(), a_commandList,
						image->Pixels.data(), static_cast<int>(image->Width),
						static_cast<int>(image->Height), texture)) {
						logger::error("Could not upload theme {}", i == 0 ? "wallpaper" : "cursor");
					}
				}
				cpu.ptr += step;
				gpu.ptr += step;
				const bool ready = texture.TextureResource != nullptr;
				if (ready) {
					hasImages = true;
					a_state.Device->CopyDescriptorsSimple(1, cpu,
						texture.ViewHeap->GetCPUDescriptorHandleForHeapStart(),
						D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				}
				const auto setTexture = i == 0 ?
					ThemeManager::SetWallpaperTexture : CursorManager::SetTexture;
				setTexture(ready ? gpu.ptr : 0, image && !ready);
			}
			if (hasImages) {
				a_state.Device->CopyDescriptorsSimple(1, heap->GetCPUDescriptorHandleForHeapStart(),
					a_state.ActiveFontResources.ViewHeap->GetCPUDescriptorHandleForHeapStart(),
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			return hasImages;
		}

		void RemapFontDescriptor(ImDrawData* a_drawData, ImTextureID a_from,
			ImTextureID a_to) noexcept
		{
			// Keep the atlas's stable public ID. Only submitted commands need the
			// descriptor from this GPU-completion-protected frame heap.
			for (auto* list : a_drawData->CmdLists) {
				for (auto& command : list->CmdBuffer) {
					if (!command.UserCallback && command.TextureId == a_from) {
						command.TextureId = a_to;
					}
				}
			}
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
			Win32Platform::IsHostWindowUsable();
		const bool enable = a_enabled && canEnable;
		if (!enable) {
			InputEventManager::SetImGuiItemActive(false);
		}
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

	bool Render(
		ID3D12GraphicsCommandList*    a_commandList,
		ID3D12Resource*               a_renderTarget,
		const DescriptorHeapSnapshot& a_engineHeaps,
		SetDescriptorHeapsFunction    a_setDescriptorHeaps,
		bool                         a_clearTarget)
	{
		if (!a_commandList || !a_renderTarget || !a_setDescriptorHeaps ||
			a_commandList->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
			!HasValidHeapSnapshot(a_engineHeaps)) {
			return false;
		}

		std::scoped_lock lock{ GetRendererMutex() };
		if (renderInProgress) {
			return false;
		}
		renderInProgress = true;
		const RenderScope renderScope{ renderInProgress };

		Microsoft::WRL::ComPtr<ID3D12Device> commandListDevice;
		if (FAILED(a_commandList->GetDevice(IID_PPV_ARGS(commandListDevice.GetAddressOf())))) {
			return false;
		}

		auto& rendererState = GetRendererState();
		if (!rendererState.Context ||
			!HasSameDeviceIdentity(rendererState.Device.Get(), commandListDevice.Get())) {
			return false;
		}

		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> commandList2;
		if (FAILED(a_commandList->QueryInterface(IID_PPV_ARGS(commandList2.GetAddressOf())))) {
			return false;
		}

		std::size_t frameSlot{};
		if (!SharedUploadsComplete(rendererState) || !AcquireFrameSlot(rendererState, frameSlot)) {
			return false;
		}

		const auto description = a_renderTarget->GetDesc();
		if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
			description.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS ||
			description.SampleDesc.Count != 1 || description.Width < 256 ||
			description.Height < 256) {
			return false;
		}

		ImGui::SetCurrentContext(rendererState.Context);
		EventManager::Snapshot lifecycleSnapshot;
		if (!EventManager::BeginFrame(lifecycleSnapshot)) {
			return false;
		}

		auto& io = ImGui::GetIO();
		if (!Win32Platform::PrepareFrame()) {
			InputEventManager::SetImGuiItemActive(false);
			return false;
		}
		ImGui_ImplDX12_NewFrame();
		FontManager::UpdateResolutionScale(io);
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
		SettingsWindow::UpdateLifecycle();
		ThemeManager::ApplyPending();
		CursorManager::Update();
		ImGui::GetStyle().MouseCursorScale =
			FrameworkSettings::GetCursorScale() * FontManager::GetActiveUIScale();
		const bool hasThemeImages = PrepareThemeImages(rendererState, a_commandList, frameSlot);

		io.DisplayFramebufferScale = ImVec2{
			static_cast<float>(description.Width) / io.DisplaySize.x,
			static_cast<float>(description.Height) / io.DisplaySize.y
		};
		const auto openGeneration = WindowManager::GetBlockingWindowOpenGeneration();
		if (!HasRecentBlockingWindowFrame(openGeneration) ||
			!WindowManager::IsBlockingWindowOpenGeneration(openGeneration)) {
			io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
			io.ClearEventsQueue();
			io.ClearInputKeys();
		} else {
			GamepadNavigation::ApplyPending(openGeneration);
		}
		EventManager::Dispatch(
			Model::EventType::kBeforeRender,
			lifecycleSnapshot);
		const auto repeatTiming = GamepadNavigation::ApplyRepeatTiming();
		ImGui::NewFrame();
		Audio::BeginFrame();
		GamepadNavigation::RestoreRepeatTiming(repeatTiming);
		HudManager::Render();
		const auto renderedGeneration = WindowManager::RenderOpenWindows();
		const auto* mainWindow = WindowManager::GetMainWindow();
		WindowPlacement::SavePending(mainWindow && mainWindow->IsOpen.load());
		InputEventManager::SetImGuiItemActive(ImGui::IsAnyItemActive());
		const bool drawMouseCursor = io.MouseDrawCursor;
		if (CursorManager::Render()) {
			io.MouseDrawCursor = false;
		}
		Audio::EndFrame();
		ImGui::Render();
		// The Win32 backend still needs this flag to hide the OS pointer.
		io.MouseDrawCursor = drawMouseCursor;

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

		auto* textureHeap = hasThemeImages ?
			rendererState.CompletionSlots[frameSlot].TextureHeap.Get() :
			rendererState.ActiveFontResources.ShaderHeap.Get();
		if (!textureHeap) {
			return false;
		}
		const auto fontDescriptor =
			reinterpret_cast<ImTextureID>(textureHeap->GetGPUDescriptorHandleForHeapStart().ptr);
		if (hasThemeImages) {
			RemapFontDescriptor(ImGui::GetDrawData(), io.Fonts->TexID, fontDescriptor);
		}
		ID3D12DescriptorHeap* frameworkHeaps[]{ textureHeap };
		a_setDescriptorHeaps(a_commandList, 1, frameworkHeaps);
		const bool recorded = ImGui_ImplDX12_RenderDrawDataChecked(
			ImGui::GetDrawData(), a_commandList, renderTargetHandle, a_clearTarget);
		if (hasThemeImages) {
			RemapFontDescriptor(ImGui::GetDrawData(), fontDescriptor, io.Fonts->TexID);
		}
		MarkFrameSlot(rendererState, commandList2.Get(), frameSlot);

		a_setDescriptorHeaps(
			a_commandList,
			a_engineHeaps.Count,
			a_engineHeaps.Heaps.data());

		if (recorded && renderedGeneration != 0) {
			renderedBlockingWindowGeneration.store(
				renderedGeneration,
				std::memory_order_release);
			lastBlockingWindowRenderTick.store(::GetTickCount64(), std::memory_order_release);
			static_cast<void>(Win32Platform::PostHostWindowCallback());
		}

		EventManager::Dispatch(
			Model::EventType::kAfterRender,
			lifecycleSnapshot);

		return recorded;
	}
}
