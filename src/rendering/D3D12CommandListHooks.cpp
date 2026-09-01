#include "rendering/RenderHooks.h"
#include "rendering/RenderHooksInternal.h"

#include "rendering/D3D12Renderer.h"

#include <RE/C/CreationRenderer.h>

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <utility>

#include <wrl/client.h>

namespace SFSEMenuFramework::RenderHooks
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using Detail::CommitHooks;
		using Detail::FunctionAddress;
		using Detail::HasMemoryAccess;
		using Detail::ReadVtableSlot;
		using Detail::RollBackHooks;
		using Detail::VtableHook;
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
		constexpr GUID        streamlineNativeInterface{
			0xADEC44E2,
			0x61F0,
			0x45C3,
			{ 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF }
		};

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
			bool           Active{ false };
			bool           SawOrdinaryTarget{ false };
			bool           SawCopyTarget{ false };
			bool           FrameStarted{ false };
			std::uint8_t   CandidateCount{ 0 };
			std::uint8_t   BarrierCallsAfterFirstCandidate{ 0 };
			PreviousRegion Previous{ PreviousRegion::Unknown };
		};

		struct DescriptorHeapState final
		{
			ComPtr<ID3D12GraphicsCommandList>             CommandList;
			std::array<ComPtr<ID3D12DescriptorHeap>, 2> Heaps;
			UINT                                       Count{ 0 };
			std::uint64_t                              Epoch{ 0 };
		};

		struct CommandListEpochEntry final
		{
			std::atomic<ID3D12GraphicsCommandList*> CommandList{ nullptr };
			std::atomic<std::uint64_t>              Epoch{ 1 };
		};

		constexpr std::size_t commandListEpochCapacity = 1024;
		static_assert((commandListEpochCapacity & (commandListEpochCapacity - 1)) == 0);
		std::array<CommandListEpochEntry, commandListEpochCapacity> commandListEpochs{};
		std::atomic<HookState>      commandListState{ HookState::Uninitialized };
		std::atomic<PreviousRegion> previousRegion{ PreviousRegion::Unknown };
		std::atomic<ResetFunction>              resetOriginal{ nullptr };
		std::atomic<ClearStateFunction>         clearStateOriginal{ nullptr };
		std::atomic<ResourceBarrierFunction>    resourceBarrierOriginal{ nullptr };
		std::atomic<SetDescriptorHeapsFunction> setDescriptorHeapsOriginal{ nullptr };

		thread_local RegionState         regionState;
		thread_local DescriptorHeapState descriptorHeapState;
		thread_local bool                internalD3D{ false };
		thread_local std::uint8_t        selfTestSeen{};
		constexpr std::uint8_t           resetSeen = 1U << 0;
		constexpr std::uint8_t           clearStateSeen = 1U << 1;
		constexpr std::uint8_t           resourceBarrierSeen = 1U << 2;
		constexpr std::uint8_t           descriptorHeapsSeen = 1U << 3;
		constexpr std::uint8_t           allSelfTestsSeen =
			resetSeen | clearStateSeen | resourceBarrierSeen | descriptorHeapsSeen;
		using CommandTargets = std::array<std::uintptr_t, commandListSlots.size()>;

		[[nodiscard]] CommandTargets ReadCommandTargets(
			const REL::Relocation<std::uintptr_t>& a_vtable)
		{
			CommandTargets result{};
			for (std::size_t index = 0; index < result.size(); ++index) {
				result[index] = ReadVtableSlot(a_vtable, commandListSlots[index]);
			}
			return result;
		}

		template <class Predicate>
		[[nodiscard]] bool AllTargets(const CommandTargets& a_targets, Predicate a_predicate)
		{
			return std::ranges::all_of(a_targets, a_predicate);
		}

		[[nodiscard]] bool GetModulePath(std::uintptr_t a_address, wchar_t (&a_path)[MAX_PATH])
		{
			if (!HasMemoryAccess(a_address, true)) {
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
			const auto length = ::GetModuleFileNameW(module, a_path, MAX_PATH);
			return length != 0 && length < MAX_PATH;
		}

		[[nodiscard]] bool IsNativeD3D12Target(std::uintptr_t a_address)
		{
			wchar_t path[MAX_PATH]{};
			if (!GetModulePath(a_address, path)) {
				return false;
			}
			const auto* fileName = std::wcsrchr(path, L'\\');
			fileName = fileName ? fileName + 1 : path;
			return ::_wcsicmp(fileName, L"d3d12.dll") == 0 ||
			       ::_wcsicmp(fileName, L"d3d12core.dll") == 0;
		}

		[[nodiscard]] bool IsAdjacentStreamlineTarget(std::uintptr_t a_address)
		{
			wchar_t ownerPath[MAX_PATH]{};
			if (!GetModulePath(a_address, ownerPath)) {
				return false;
			}
			auto* ownerFileName = std::wcsrchr(ownerPath, L'\\');
			if (!ownerFileName || ::_wcsicmp(ownerFileName + 1, L"sl.interposer.dll") != 0) {
				return false;
			}
			*ownerFileName = L'\0';

			wchar_t executablePath[MAX_PATH]{};
			const auto executableLength = ::GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
			if (executableLength == 0 || executableLength >= MAX_PATH) {
				return false;
			}

			auto* executableFileName = std::wcsrchr(executablePath, L'\\');
			if (!executableFileName) {
				return false;
			}
			*executableFileName = L'\0';
			return ::_wcsicmp(ownerPath, executablePath) == 0;
		}

		template <class T>
		[[nodiscard]] bool GetStreamlineNativeInterface(T* a_interface, ComPtr<T>& a_native) noexcept
		{
			a_native.Reset();
			if (!a_interface) {
				return false;
			}

			T* native{};
			if (FAILED(a_interface->QueryInterface(
					streamlineNativeInterface,
					reinterpret_cast<void**>(&native))) ||
				!native) {
				return false;
			}

			a_native.Attach(native);
			if (native == a_interface) {
				a_native.Reset();
				return false;
			}
			return true;
		}
		[[nodiscard]] std::uint64_t CommandListEpoch(
			ID3D12GraphicsCommandList* a_commandList,
			bool                       a_advance = false) noexcept
		{
			if (!a_commandList) {
				return 0;
			}
			const auto start =
				(reinterpret_cast<std::uintptr_t>(a_commandList) >> 4) &
				(commandListEpochCapacity - 1);
			for (std::size_t offset = 0; offset < commandListEpochCapacity; ++offset) {
				auto& entry = commandListEpochs[(start + offset) & (commandListEpochCapacity - 1)];
				auto* current = entry.CommandList.load(std::memory_order_acquire);
				if (!current) {
					auto* expected = static_cast<ID3D12GraphicsCommandList*>(nullptr);
					entry.CommandList.compare_exchange_strong(
						expected,
						a_commandList,
						std::memory_order_acq_rel,
						std::memory_order_acquire);
					current = expected ? expected : a_commandList;
				}
				if (current == a_commandList) {
					return a_advance ?
						entry.Epoch.fetch_add(1, std::memory_order_acq_rel) + 1 :
						entry.Epoch.load(std::memory_order_acquire);
				}
			}
			return 0;
		}

		[[nodiscard]] bool CopyHeapSnapshot(
			ID3D12GraphicsCommandList*             a_commandList,
			D3D12Renderer::DescriptorHeapSnapshot& a_snapshot) noexcept
		{
			if (!descriptorHeapState.Count || descriptorHeapState.CommandList.Get() != a_commandList) {
				return false;
			}

			const auto epoch = CommandListEpoch(a_commandList);
			if (!epoch || descriptorHeapState.Epoch != epoch) {
				descriptorHeapState = {};
				return false;
			}

			a_snapshot = {};
			a_snapshot.Count = descriptorHeapState.Count;
			for (UINT index = 0; index < descriptorHeapState.Count; ++index) {
				a_snapshot.Heaps[index] = descriptorHeapState.Heaps[index].Get();
			}
			if (CommandListEpoch(a_commandList) != epoch) {
				descriptorHeapState = {};
				a_snapshot = {};
				return false;
			}
			return true;
		}

		[[nodiscard]] bool IsCandidateResource(ID3D12Resource* a_resource) noexcept
		{
			if (!a_resource) {
				return false;
			}

			const auto description = a_resource->GetDesc();
			return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
			       description.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
			       description.DepthOrArraySize == 1 && description.MipLevels == 1 &&
			       description.SampleDesc.Count == 1 && description.SampleDesc.Quality == 0 &&
			       description.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && description.Width >= 256 &&
			       description.Height >= 256 &&
			       (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
		}

		void InspectBarrierCandidate(
			ID3D12GraphicsCommandList*    a_commandList,
			const D3D12_RESOURCE_BARRIER& a_barrier) noexcept
		{
			if (!regionState.Active || a_barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
				a_barrier.Flags != D3D12_RESOURCE_BARRIER_FLAG_NONE) {
				return;
			}
			if (a_barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET) {
				return;
			}
			if (!IsCandidateResource(a_barrier.Transition.pResource)) {
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
				(copyTarget && regionState.SawCopyTarget) ||
				regionState.CandidateCount >= 2) {
				return;
			}
			++regionState.CandidateCount;

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

			internalD3D = true;
			D3D12Renderer::Render(
				a_commandList,
				a_barrier.Transition.pResource,
				heapSnapshot,
				setDescriptorHeapsOriginal.load(std::memory_order_acquire));
			internalD3D = false;
		}

		void STDMETHODCALLTYPE ResourceBarrierThunk(
			ID3D12GraphicsCommandList*    a_commandList,
			UINT                          a_barrierCount,
			const D3D12_RESOURCE_BARRIER* a_barriers) noexcept
		{
			const auto original = resourceBarrierOriginal.load(std::memory_order_acquire);

			if (internalD3D) {
				selfTestSeen |= resourceBarrierSeen;
				original(a_commandList, a_barrierCount, a_barriers);
				return;
			}

			if (commandListState.load(std::memory_order_acquire) == HookState::Ready &&
				regionState.Active &&
				a_commandList && a_commandList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT &&
				a_barriers) {
				if (!regionState.CandidateCount ||
					regionState.BarrierCallsAfterFirstCandidate < 4) {
					regionState.BarrierCallsAfterFirstCandidate += regionState.CandidateCount != 0;
					for (UINT index = 0; index < a_barrierCount; ++index) {
						InspectBarrierCandidate(a_commandList, a_barriers[index]);
					}
				}
			}

			original(a_commandList, a_barrierCount, a_barriers);
		}

		void InvalidateHeapCapture(ID3D12GraphicsCommandList* a_commandList) noexcept
		{
			if (a_commandList && a_commandList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
				static_cast<void>(CommandListEpoch(a_commandList, true));
			}
			if (descriptorHeapState.CommandList.Get() == a_commandList) {
				descriptorHeapState = {};
			}
		}

		HRESULT STDMETHODCALLTYPE ResetThunk(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12CommandAllocator*     a_allocator,
			ID3D12PipelineState*        a_initialState) noexcept
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
			ID3D12PipelineState*        a_pipelineState) noexcept
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
			ID3D12GraphicsCommandList*   a_commandList,
			UINT                         a_heapCount,
			ID3D12DescriptorHeap* const* a_heaps) noexcept
		{
			const auto original = setDescriptorHeapsOriginal.load(std::memory_order_acquire);

			original(a_commandList, a_heapCount, a_heaps);
			if (internalD3D) {
				selfTestSeen |= descriptorHeapsSeen;
				return;
			}

			const bool directCommandList =
				a_commandList && a_commandList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (directCommandList) {
				static_cast<void>(CommandListEpoch(a_commandList, true));
			}
			DescriptorHeapState nextState;
			if (!directCommandList || !a_heaps || a_heapCount == 0 ||
				a_heapCount > nextState.Heaps.size()) {
				descriptorHeapState = {};
				return;
			}
			const auto epoch = CommandListEpoch(a_commandList);
			if (!epoch) {
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
			nextState.Epoch = epoch;
			if (CommandListEpoch(a_commandList) != epoch) {
				descriptorHeapState = {};
				return;
			}
			descriptorHeapState = std::move(nextState);
		}
	}

	bool Detail::EnsureCommandListHooks() noexcept
	{
		auto state = commandListState.load(std::memory_order_acquire);
		if (state == HookState::Ready) {
			return true;
		}
		if (state == HookState::Installing || state == HookState::Failed) {
			return false;
		}

		HookState expectedState = HookState::Uninitialized;
		if (!commandListState.compare_exchange_strong(
				expectedState,
				HookState::Installing,
				std::memory_order_acq_rel)) {
			return expectedState == HookState::Ready;
		}

		auto fail = []() noexcept {
			commandListState.store(HookState::Failed, std::memory_order_release);
			return false;
		};

		auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
		if (!renderer || !renderer->GetDevice()) {
			commandListState.store(HookState::Uninitialized, std::memory_order_release);
			return false;
		}

		ComPtr<ID3D12Device> device;
		auto*                borrowedDevice = reinterpret_cast<ID3D12Device*>(renderer->GetDevice());
		if (FAILED(borrowedDevice->QueryInterface(IID_PPV_ARGS(device.GetAddressOf())))) {
			logger::critical("Could not acquire the Starfield DirectX 12 device");
			return fail();
		}

		ComPtr<ID3D12CommandAllocator> allocator;
		if (FAILED(device->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(allocator.GetAddressOf())))) {
			logger::critical("Failed to create the D3D12 hook-test allocator");
			return fail();
		}

		ComPtr<ID3D12GraphicsCommandList> commandList;
		if (FAILED(device->CreateCommandList(
				0,
				D3D12_COMMAND_LIST_TYPE_DIRECT,
				allocator.Get(),
				nullptr,
				IID_PPV_ARGS(commandList.GetAddressOf())))) {
			logger::critical("Failed to create the D3D12 hook-test command list");
			return fail();
		}

		ComPtr<ID3D12GraphicsCommandList> nativeCommandList;
		const bool streamlineProxy = GetStreamlineNativeInterface(
			commandList.Get(),
			nativeCommandList);
		if (streamlineProxy) {
			auto* proxyRawVtable = *reinterpret_cast<std::uintptr_t**>(commandList.Get());
			if (!proxyRawVtable) {
				logger::critical("The NVIDIA Streamline command-list vtable is unavailable");
				return fail();
			}

			REL::Relocation<std::uintptr_t> proxyVtable{
				reinterpret_cast<std::uintptr_t>(proxyRawVtable)
			};
			const auto proxyTargets = ReadCommandTargets(proxyVtable);
			if (!AllTargets(proxyTargets, IsAdjacentStreamlineTarget)) {
				logger::critical("Unsupported NVIDIA Streamline command-list vtable");
				return fail();
			}
		}
		auto* hookCommandList = streamlineProxy ? nativeCommandList.Get() : commandList.Get();

		ComPtr<ID3D12Device> hookDevice;
		if (FAILED(hookCommandList->GetDevice(IID_PPV_ARGS(hookDevice.GetAddressOf())))) {
			logger::critical("Could not acquire the native D3D12 command-list device");
			return fail();
		}

		ComPtr<ID3D12Device> nativeRendererDevice;
		const bool rendererDeviceProxy = GetStreamlineNativeInterface(
			device.Get(),
			nativeRendererDevice);
		auto* expectedHookDevice = rendererDeviceProxy ? nativeRendererDevice.Get() : device.Get();
		if (!D3D12Renderer::HasSameDeviceIdentity(
				hookDevice.Get(), expectedHookDevice)) {
			logger::critical("The native D3D12 command list and renderer device do not match");
			return fail();
		}

		if (streamlineProxy) {
			logger::info("NVIDIA Streamline command-list proxy detected; using its native interface");
		}

		auto* rawVtable = *reinterpret_cast<std::uintptr_t**>(hookCommandList);
		if (!rawVtable) {
			logger::critical("The D3D12 command-list vtable is unavailable");
			return fail();
		}

		REL::Relocation<std::uintptr_t> vtable{ reinterpret_cast<std::uintptr_t>(rawVtable) };
		const auto targets = ReadCommandTargets(vtable);
		const CommandTargets replacements{
			FunctionAddress(&ResetThunk),
			FunctionAddress(&ClearStateThunk),
			FunctionAddress(&ResourceBarrierThunk),
			FunctionAddress(&SetDescriptorHeapsThunk)
		};
		bool targetsValid = AllTargets(targets, IsNativeD3D12Target);
		for (std::size_t index = 0; index < targets.size(); ++index) {
			targetsValid = targetsValid && targets[index] != replacements[index];
		}
		if (!targetsValid) {
			logger::critical("Unsupported native D3D12 command-list targets");
			return fail();
		}

		resetOriginal.store(reinterpret_cast<ResetFunction>(targets[0]), std::memory_order_release);
		clearStateOriginal.store(reinterpret_cast<ClearStateFunction>(targets[1]), std::memory_order_release);
		resourceBarrierOriginal.store(
			reinterpret_cast<ResourceBarrierFunction>(targets[2]), std::memory_order_release);
		setDescriptorHeapsOriginal.store(
			reinterpret_cast<SetDescriptorHeapsFunction>(targets[3]), std::memory_order_release);

		std::array<VtableHook, 4> hooks{
			VtableHook{ &vtable, commandListSlots[0], targets[0], replacements[0] },
			VtableHook{ &vtable, commandListSlots[1], targets[1], replacements[1] },
			VtableHook{ &vtable, commandListSlots[2], targets[2], replacements[2] },
			VtableHook{ &vtable, commandListSlots[3], targets[3], replacements[3] }
		};
		if (!CommitHooks(hooks)) {
			const bool restored = RollBackHooks(hooks);
			logger::critical(
				"Failed to commit the D3D12 command-list hooks; rollback {}",
				restored ? "succeeded" : "was incomplete");
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
			const bool restored = RollBackHooks(hooks);
			logger::critical(
				"The D3D12 hook self-test failed; rollback {}",
				restored ? "succeeded" : "was incomplete");
			return fail();
		}

		internalD3D = true;
		const bool rendererReady = D3D12Renderer::Initialize(hookDevice.Get());
		internalD3D = false;
		if (!rendererReady) {
			const bool restored = RollBackHooks(hooks);
			logger::critical(
				"The ImGui renderer could not be initialized; D3D12 rollback {}",
				restored ? "succeeded" : "was incomplete");
			return fail();
		}

		commandListState.store(HookState::Ready, std::memory_order_release);
		logger::info("D3D12 command-list hooks installed and self-tested");
		return true;
	}

	bool HasTerminalRendererFailure() noexcept
	{
		return commandListState.load(std::memory_order_acquire) ==
		       HookState::Failed;
	}

	void Detail::ResetRegion() noexcept
	{
		regionState = {};
	}

	void Detail::ActivateRegionAfterScaleformEnd() noexcept
	{
		regionState = {};
		regionState.Active = true;
		regionState.Previous =
			previousRegion.load(std::memory_order_acquire);
	}

	void Detail::FinalizeRegionBeforeComposite() noexcept
	{
		if (regionState.Active && regionState.CandidateCount) {
			previousRegion.store(
				regionState.SawCopyTarget ?
					PreviousRegion::FrameGeneration :
					PreviousRegion::Normal,
				std::memory_order_release);
		}
		ResetRegion();
	}
}
