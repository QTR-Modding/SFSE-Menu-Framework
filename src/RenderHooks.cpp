#include "RenderHooks.h"

#include "D3D12Renderer.h"

#include <RE/C/CreationRenderer.h>

#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <utility>

#include <wrl/client.h>

namespace SFSEMenuFramework::RenderHooks
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		using RenderPassFunction = RE::CreationRendererPrivate::ExecuteRenderPass_t*;
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

		constexpr std::size_t resetIndex = 10;
		constexpr std::size_t clearStateIndex = 11;
		constexpr std::size_t resourceBarrierIndex = 26;
		constexpr std::size_t setDescriptorHeapsIndex = 28;
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
			bool           SawValidCandidate{ false };
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
			bool                                       Valid{ false };
		};

		struct CommandListEpochEntry final
		{
			std::atomic<ID3D12GraphicsCommandList*> CommandList{ nullptr };
			std::atomic<std::uint64_t>              Epoch{ 1 };
		};

		constexpr std::size_t commandListEpochCapacity = 1024;
		static_assert((commandListEpochCapacity & (commandListEpochCapacity - 1)) == 0);
		std::array<CommandListEpochEntry, commandListEpochCapacity> commandListEpochs{};

		std::atomic<HookState>      scaleformState{ HookState::Uninitialized };
		std::atomic<HookState>      commandListState{ HookState::Uninitialized };
		std::atomic<bool>           drawEnabled{ false };
		std::atomic<PreviousRegion> previousRegion{ PreviousRegion::Unknown };

		std::atomic<RenderPassFunction>         beginOriginal{ nullptr };
		std::atomic<RenderPassFunction>         endOriginal{ nullptr };
		std::atomic<RenderPassFunction>         compositeOriginal{ nullptr };
		std::atomic<ResetFunction>              resetOriginal{ nullptr };
		std::atomic<ClearStateFunction>         clearStateOriginal{ nullptr };
		std::atomic<ResourceBarrierFunction>    resourceBarrierOriginal{ nullptr };
		std::atomic<SetDescriptorHeapsFunction> setDescriptorHeapsOriginal{ nullptr };

		std::atomic<std::uint64_t> realResourceBarrierHits{ 0 };
		std::atomic<std::uint64_t> realDescriptorHeapHits{ 0 };
		std::atomic<std::uint64_t> completedRegions{ 0 };
		std::atomic<std::uint64_t> renderedRegions{ 0 };
		std::atomic<std::uint64_t> beginPassHits{ 0 };
		std::atomic<std::uint64_t> endPassHits{ 0 };
		std::atomic<std::uint64_t> compositePassHits{ 0 };
		std::atomic<std::uint64_t> activeBarrierCalls{ 0 };
		std::atomic<std::uint64_t> activeDescriptorHeapHits{ 0 };
		std::atomic<std::uint64_t> transitionBarrierHits{ 0 };
		std::atomic<std::uint64_t> renderTargetExitHits{ 0 };
		std::atomic<std::uint64_t> candidateShapeHits{ 0 };
		std::atomic<std::uint64_t> ordinaryCandidateHits{ 0 };
		std::atomic<std::uint64_t> copyCandidateHits{ 0 };
		std::atomic<std::uint64_t> selectedCandidateHits{ 0 };
		std::atomic<std::uint64_t> missingHeapSnapshotHits{ 0 };
		std::atomic<std::uint64_t> renderAttempts{ 0 };
		std::atomic<std::uint64_t> probeFrames{ 0 };
		std::atomic<std::uint64_t> realResetHits{ 0 };
		std::atomic<std::uint64_t> realClearStateHits{ 0 };
		std::atomic<std::uint64_t> heapSnapshotInvalidations{ 0 };
		std::atomic<std::uint64_t> heapSnapshotEpochMismatches{ 0 };
		std::atomic<std::uint64_t> epochTableExhaustions{ 0 };

		constexpr auto renderResultCount =
			static_cast<std::size_t>(D3D12Renderer::RenderResult::Count);
		std::array<std::atomic<std::uint64_t>, renderResultCount> renderResults{};

		thread_local RegionState         regionState;
		thread_local DescriptorHeapState descriptorHeapState;
		thread_local bool                internalD3D{ false };
		thread_local bool                selfTestResetSeen{ false };
		thread_local bool                selfTestClearStateSeen{ false };
		thread_local bool                selfTestResourceBarrierSeen{ false };
		thread_local bool                selfTestDescriptorHeapsSeen{ false };

		void BeginThunk(
			RE::CreationRendererPrivate::RenderPass*              a_pass,
			RE::CreationRendererPrivate::RenderPassContext*       a_context,
			RE::CreationRendererPrivate::RenderPassExecutionData* a_executionData) noexcept;
		void EndThunk(
			RE::CreationRendererPrivate::RenderPass*              a_pass,
			RE::CreationRendererPrivate::RenderPassContext*       a_context,
			RE::CreationRendererPrivate::RenderPassExecutionData* a_executionData) noexcept;
		void CompositeThunk(
			RE::CreationRendererPrivate::RenderPass*              a_pass,
			RE::CreationRendererPrivate::RenderPassContext*       a_context,
			RE::CreationRendererPrivate::RenderPassExecutionData* a_executionData) noexcept;
		void STDMETHODCALLTYPE ResourceBarrierThunk(
			ID3D12GraphicsCommandList*    a_commandList,
			UINT                          a_barrierCount,
			const D3D12_RESOURCE_BARRIER* a_barriers) noexcept;
		HRESULT STDMETHODCALLTYPE ResetThunk(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12CommandAllocator*     a_allocator,
			ID3D12PipelineState*        a_initialState) noexcept;
		void STDMETHODCALLTYPE ClearStateThunk(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12PipelineState*        a_pipelineState) noexcept;
		void STDMETHODCALLTYPE SetDescriptorHeapsThunk(
			ID3D12GraphicsCommandList*   a_commandList,
			UINT                         a_heapCount,
			ID3D12DescriptorHeap* const* a_heaps) noexcept;

		[[nodiscard]] std::uintptr_t FunctionAddress(RenderPassFunction a_function)
		{
			return reinterpret_cast<std::uintptr_t>(a_function);
		}

		[[nodiscard]] std::uintptr_t FunctionAddress(ResourceBarrierFunction a_function)
		{
			return reinterpret_cast<std::uintptr_t>(a_function);
		}

		[[nodiscard]] std::uintptr_t FunctionAddress(ResetFunction a_function)
		{
			return reinterpret_cast<std::uintptr_t>(a_function);
		}

		[[nodiscard]] std::uintptr_t FunctionAddress(ClearStateFunction a_function)
		{
			return reinterpret_cast<std::uintptr_t>(a_function);
		}

		[[nodiscard]] std::uintptr_t FunctionAddress(SetDescriptorHeapsFunction a_function)
		{
			return reinterpret_cast<std::uintptr_t>(a_function);
		}

		[[nodiscard]] bool IsReadableAddress(std::uintptr_t a_address)
		{
			if (!a_address || a_address % alignof(std::uintptr_t) != 0) {
				return false;
			}

			MEMORY_BASIC_INFORMATION memory{};
			if (::VirtualQuery(reinterpret_cast<const void*>(a_address), &memory, sizeof(memory)) == 0 ||
				memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0 ||
				(memory.Protect & 0xFF) == PAGE_NOACCESS) {
				return false;
			}

			const auto regionEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
			return a_address + sizeof(std::uintptr_t) <= regionEnd;
		}

		[[nodiscard]] bool IsExecutableAddress(std::uintptr_t a_address)
		{
			MEMORY_BASIC_INFORMATION memory{};
			if (!a_address ||
				::VirtualQuery(reinterpret_cast<const void*>(a_address), &memory, sizeof(memory)) == 0 ||
				memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0) {
				return false;
			}

			switch (memory.Protect & 0xFF) {
			case PAGE_EXECUTE:
			case PAGE_EXECUTE_READ:
			case PAGE_EXECUTE_READWRITE:
			case PAGE_EXECUTE_WRITECOPY:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] std::uintptr_t ReadVtableSlot(
			const REL::Relocation<std::uintptr_t>& a_vtable,
			std::size_t                            a_index)
		{
			const auto address = a_vtable.address() + sizeof(std::uintptr_t) * a_index;
			if (!IsReadableAddress(address)) {
				return 0;
			}
			return *reinterpret_cast<const std::uintptr_t*>(address);
		}

		struct AddressModule final
		{
			HMODULE Module{};
			wchar_t Path[MAX_PATH]{};
		};

		[[nodiscard]] bool GetAddressModule(std::uintptr_t a_address, AddressModule& a_result)
		{
			if (!IsExecutableAddress(a_address)) {
				return false;
			}

			if (!::GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
						GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address),
					&a_result.Module)) {
				return false;
			}

			const auto length = ::GetModuleFileNameW(a_result.Module, a_result.Path, MAX_PATH);
			if (length == 0 || length >= MAX_PATH) {
				return false;
			}
			return true;
		}

		[[nodiscard]] bool IsNativeD3D12Target(std::uintptr_t a_address)
		{
			AddressModule owner{};
			if (!GetAddressModule(a_address, owner)) {
				return false;
			}

			const auto* fileName = std::wcsrchr(owner.Path, L'\\');
			fileName = fileName ? fileName + 1 : owner.Path;
			return ::_wcsicmp(fileName, L"d3d12.dll") == 0 ||
			       ::_wcsicmp(fileName, L"d3d12core.dll") == 0;
		}

		[[nodiscard]] bool IsAdjacentStreamlineTarget(std::uintptr_t a_address)
		{
			AddressModule owner{};
			if (!GetAddressModule(a_address, owner)) {
				return false;
			}

			auto* ownerFileName = std::wcsrchr(owner.Path, L'\\');
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
			return ::_wcsicmp(owner.Path, executablePath) == 0;
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

		[[nodiscard]] bool WriteVtableSlot(
			REL::Relocation<std::uintptr_t>& a_vtable,
			std::size_t                      a_index,
			std::uintptr_t                   a_expected,
			std::uintptr_t                   a_replacement,
			std::uintptr_t&                  a_previous,
			bool&                            a_attempted,
			bool&                            a_written)
		{
			if (ReadVtableSlot(a_vtable, a_index) != a_expected) {
				return false;
			}

			a_attempted = true;
			a_previous = a_vtable.write_vfunc(a_index, a_replacement);
			a_written = ReadVtableSlot(a_vtable, a_index) == a_replacement;
			return a_previous == a_expected && a_written;
		}

		[[nodiscard]] bool RestoreVtableSlot(
			REL::Relocation<std::uintptr_t>& a_vtable,
			std::size_t                      a_index,
			std::uintptr_t                   a_ours,
			std::uintptr_t                   a_previous,
			bool                             a_attempted,
			bool                             a_written)
		{
			if (!a_attempted) {
				return true;
			}

			const auto current = ReadVtableSlot(a_vtable, a_index);
			if (!a_written && current == a_previous) {
				return true;
			}
			if (current == a_previous) {
				return true;
			}
			if (current != a_ours) {
				return false;
			}

			a_vtable.write_vfunc(a_index, a_previous);
			return ReadVtableSlot(a_vtable, a_index) == a_previous;
		}

		void ResetRegion() noexcept
		{
			regionState = {};
		}

		[[nodiscard]] CommandListEpochEntry* GetCommandListEpochEntry(
			ID3D12GraphicsCommandList* a_commandList) noexcept
		{
			if (!a_commandList) {
				return nullptr;
			}

			const auto start =
				(reinterpret_cast<std::uintptr_t>(a_commandList) >> 4) &
				(commandListEpochCapacity - 1);
			for (std::size_t offset = 0; offset < commandListEpochCapacity; ++offset) {
				auto& entry = commandListEpochs[(start + offset) & (commandListEpochCapacity - 1)];
				auto* current = entry.CommandList.load(std::memory_order_acquire);
				if (current == a_commandList) {
					return &entry;
				}
				if (!current) {
					auto* expected = static_cast<ID3D12GraphicsCommandList*>(nullptr);
					if (entry.CommandList.compare_exchange_strong(
							expected,
							a_commandList,
							std::memory_order_acq_rel,
							std::memory_order_acquire) ||
						expected == a_commandList) {
						return &entry;
					}
				}
			}

			epochTableExhaustions.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}

		[[nodiscard]] std::uint64_t ReadCommandListEpoch(
			ID3D12GraphicsCommandList* a_commandList) noexcept
		{
			const auto* entry = GetCommandListEpochEntry(a_commandList);
			return entry ? entry->Epoch.load(std::memory_order_acquire) : 0;
		}

		void AdvanceCommandListEpoch(ID3D12GraphicsCommandList* a_commandList) noexcept
		{
			if (auto* entry = GetCommandListEpochEntry(a_commandList)) {
				entry->Epoch.fetch_add(1, std::memory_order_acq_rel);
			}
		}

		void MaybeLogRenderProbe() noexcept
		{
			const auto frame = probeFrames.fetch_add(1, std::memory_order_relaxed) + 1;
			if (frame != 1 && frame != 60 && frame != 300) {
				return;
			}

			auto load = [](const auto& a_counter) {
				return a_counter.load(std::memory_order_relaxed);
			};
			logger::info(
				"Render probe {}: passes={}/{}/{}, regions={}, barriers={}/{}, heaps={}/{}, "
				"transitions={}, rt-exits={}, shape={}, ordinary={}, copy={}, selected={}, "
				"missing-heap={}, attempts={}, rendered={}, resets={}, clear-states={}, "
				"heap-invalidations={}, epoch-mismatches={}, epoch-table-full={}",
				frame,
				load(beginPassHits),
				load(endPassHits),
				load(compositePassHits),
				load(completedRegions),
				load(realResourceBarrierHits),
				load(activeBarrierCalls),
				load(realDescriptorHeapHits),
				load(activeDescriptorHeapHits),
				load(transitionBarrierHits),
				load(renderTargetExitHits),
				load(candidateShapeHits),
				load(ordinaryCandidateHits),
				load(copyCandidateHits),
				load(selectedCandidateHits),
				load(missingHeapSnapshotHits),
				load(renderAttempts),
				load(renderedRegions),
				load(realResetHits),
				load(realClearStateHits),
				load(heapSnapshotInvalidations),
				load(heapSnapshotEpochMismatches),
				load(epochTableExhaustions));
			logger::info(
				"Render outcomes: invalid-args={}, busy={}, device-query={}, device-mismatch={}, "
				"list2={}, slot-busy={}, invalid-target={}, platform-frame={}, display-size={}",
				load(renderResults[static_cast<std::size_t>(
					D3D12Renderer::RenderResult::InvalidArguments)]),
				load(renderResults[static_cast<std::size_t>(D3D12Renderer::RenderResult::Busy)]),
				load(renderResults[static_cast<std::size_t>(
					D3D12Renderer::RenderResult::DeviceQueryFailed)]),
				load(renderResults[static_cast<std::size_t>(
					D3D12Renderer::RenderResult::DeviceMismatch)]),
				load(renderResults[static_cast<std::size_t>(
					D3D12Renderer::RenderResult::CommandList2Unavailable)]),
				load(renderResults[static_cast<std::size_t>(
					D3D12Renderer::RenderResult::FrameSlotBusy)]),
				load(renderResults[static_cast<std::size_t>(
					D3D12Renderer::RenderResult::InvalidTarget)]),
				load(renderResults[static_cast<std::size_t>(
					D3D12Renderer::RenderResult::PlatformFrameUnavailable)]),
				load(renderResults[static_cast<std::size_t>(
					D3D12Renderer::RenderResult::InvalidDisplaySize)]));
		}

		[[nodiscard]] bool CopyHeapSnapshot(
			ID3D12GraphicsCommandList*             a_commandList,
			D3D12Renderer::DescriptorHeapSnapshot& a_snapshot) noexcept
		{
			if (!descriptorHeapState.Valid ||
				descriptorHeapState.CommandList.Get() != a_commandList) {
				return false;
			}

			const auto epoch = ReadCommandListEpoch(a_commandList);
			if (!epoch || descriptorHeapState.Epoch != epoch) {
				heapSnapshotEpochMismatches.fetch_add(1, std::memory_order_relaxed);
				descriptorHeapState = {};
				return false;
			}

			a_snapshot = {};
			a_snapshot.Count = descriptorHeapState.Count;
			for (UINT index = 0; index < descriptorHeapState.Count; ++index) {
				a_snapshot.Heaps[index] = descriptorHeapState.Heaps[index].Get();
			}
			if (ReadCommandListEpoch(a_commandList) != epoch) {
				heapSnapshotEpochMismatches.fetch_add(1, std::memory_order_relaxed);
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
			transitionBarrierHits.fetch_add(1, std::memory_order_relaxed);
			if (a_barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET) {
				return;
			}
			renderTargetExitHits.fetch_add(1, std::memory_order_relaxed);
			if (!IsCandidateResource(a_barrier.Transition.pResource)) {
				return;
			}
			candidateShapeHits.fetch_add(1, std::memory_order_relaxed);

			const auto stateAfter = a_barrier.Transition.StateAfter;
			const bool ordinaryTarget =
				(stateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0;
			const bool copyTarget =
				(stateAfter & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0 && !ordinaryTarget;
			if (ordinaryTarget) {
				ordinaryCandidateHits.fetch_add(1, std::memory_order_relaxed);
			}
			if (copyTarget) {
				copyCandidateHits.fetch_add(1, std::memory_order_relaxed);
			}
			if (!ordinaryTarget && !copyTarget) {
				return;
			}
			if ((ordinaryTarget && regionState.SawOrdinaryTarget) ||
				(copyTarget && regionState.SawCopyTarget) ||
				regionState.CandidateCount >= 2) {
				return;
			}
			++regionState.CandidateCount;

			regionState.SawValidCandidate = true;
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
			selectedCandidateHits.fetch_add(1, std::memory_order_relaxed);

			regionState.FrameStarted = true;
			D3D12Renderer::DescriptorHeapSnapshot heapSnapshot;
			if (!CopyHeapSnapshot(a_commandList, heapSnapshot)) {
				missingHeapSnapshotHits.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			const auto setHeaps = setDescriptorHeapsOriginal.load(std::memory_order_acquire);
			if (!setHeaps) {
				return;
			}

			internalD3D = true;
			renderAttempts.fetch_add(1, std::memory_order_relaxed);
			const auto result = D3D12Renderer::Render(
				a_commandList,
				a_barrier.Transition.pResource,
				heapSnapshot,
				setHeaps);
			internalD3D = false;
			renderResults[static_cast<std::size_t>(result)].fetch_add(
				1,
				std::memory_order_relaxed);

			if (result == D3D12Renderer::RenderResult::Rendered) {
				renderedRegions.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void STDMETHODCALLTYPE ResourceBarrierThunk(
			ID3D12GraphicsCommandList*    a_commandList,
			UINT                          a_barrierCount,
			const D3D12_RESOURCE_BARRIER* a_barriers) noexcept
		{
			const auto original = resourceBarrierOriginal.load(std::memory_order_acquire);
			if (!original) {
				return;
			}

			if (internalD3D) {
				selfTestResourceBarrierSeen = true;
				original(a_commandList, a_barrierCount, a_barriers);
				return;
			}

			realResourceBarrierHits.fetch_add(1, std::memory_order_relaxed);
			if (drawEnabled.load(std::memory_order_acquire) && regionState.Active &&
				a_commandList && a_commandList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT &&
				a_barriers) {
				activeBarrierCalls.fetch_add(1, std::memory_order_relaxed);
				if (regionState.SawValidCandidate) {
					if (regionState.BarrierCallsAfterFirstCandidate >= 4) {
						original(a_commandList, a_barrierCount, a_barriers);
						return;
					}
					++regionState.BarrierCallsAfterFirstCandidate;
				}
				for (UINT index = 0; index < a_barrierCount; ++index) {
					InspectBarrierCandidate(a_commandList, a_barriers[index]);
				}
			}

			original(a_commandList, a_barrierCount, a_barriers);
		}

		HRESULT STDMETHODCALLTYPE ResetThunk(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12CommandAllocator*     a_allocator,
			ID3D12PipelineState*        a_initialState) noexcept
		{
			const auto original = resetOriginal.load(std::memory_order_acquire);
			if (!original) {
				return E_FAIL;
			}

			if (internalD3D) {
				selfTestResetSeen = true;
				return original(a_commandList, a_allocator, a_initialState);
			}

			realResetHits.fetch_add(1, std::memory_order_relaxed);
			const auto result = original(a_commandList, a_allocator, a_initialState);
			if (SUCCEEDED(result)) {
				if (a_commandList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
					AdvanceCommandListEpoch(a_commandList);
				}
				if (descriptorHeapState.CommandList.Get() == a_commandList) {
					descriptorHeapState = {};
					heapSnapshotInvalidations.fetch_add(1, std::memory_order_relaxed);
				}
			}
			return result;
		}

		void STDMETHODCALLTYPE ClearStateThunk(
			ID3D12GraphicsCommandList* a_commandList,
			ID3D12PipelineState*        a_pipelineState) noexcept
		{
			const auto original = clearStateOriginal.load(std::memory_order_acquire);
			if (!original) {
				return;
			}

			if (internalD3D) {
				selfTestClearStateSeen = true;
				original(a_commandList, a_pipelineState);
				return;
			}

			original(a_commandList, a_pipelineState);
			realClearStateHits.fetch_add(1, std::memory_order_relaxed);
			if (a_commandList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT) {
				AdvanceCommandListEpoch(a_commandList);
			}
			if (descriptorHeapState.CommandList.Get() == a_commandList) {
				descriptorHeapState = {};
				heapSnapshotInvalidations.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void STDMETHODCALLTYPE SetDescriptorHeapsThunk(
			ID3D12GraphicsCommandList*   a_commandList,
			UINT                         a_heapCount,
			ID3D12DescriptorHeap* const* a_heaps) noexcept
		{
			const auto original = setDescriptorHeapsOriginal.load(std::memory_order_acquire);
			if (!original) {
				return;
			}

			original(a_commandList, a_heapCount, a_heaps);
			if (internalD3D) {
				selfTestDescriptorHeapsSeen = true;
				return;
			}

			realDescriptorHeapHits.fetch_add(1, std::memory_order_relaxed);
			if (regionState.Active) {
				activeDescriptorHeapHits.fetch_add(1, std::memory_order_relaxed);
			}
			const bool directCommandList =
				a_commandList && a_commandList->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (directCommandList) {
				AdvanceCommandListEpoch(a_commandList);
			}
			DescriptorHeapState nextState;
			if (!directCommandList || !a_heaps || a_heapCount == 0 ||
				a_heapCount > nextState.Heaps.size()) {
				descriptorHeapState = {};
				return;
			}
			const auto epoch = ReadCommandListEpoch(a_commandList);
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
			nextState.Valid = true;
			if (ReadCommandListEpoch(a_commandList) != epoch) {
				descriptorHeapState = {};
				return;
			}
			descriptorHeapState = std::move(nextState);
		}

		[[nodiscard]] bool RollBackCommandListHooks(
			REL::Relocation<std::uintptr_t>& a_vtable,
			bool                             a_heapAttempted,
			bool                             a_heapWritten,
			std::uintptr_t                   a_heapPrevious,
			bool                             a_barrierAttempted,
			bool                             a_barrierWritten,
			std::uintptr_t                   a_barrierPrevious,
			bool                             a_clearStateAttempted,
			bool                             a_clearStateWritten,
			std::uintptr_t                   a_clearStatePrevious,
			bool                             a_resetAttempted,
			bool                             a_resetWritten,
			std::uintptr_t                   a_resetPrevious)
		{
			const bool heapRestored = RestoreVtableSlot(
				a_vtable,
				setDescriptorHeapsIndex,
				FunctionAddress(&SetDescriptorHeapsThunk),
				a_heapPrevious,
				a_heapAttempted,
				a_heapWritten);
			const bool barrierRestored = RestoreVtableSlot(
				a_vtable,
				resourceBarrierIndex,
				FunctionAddress(&ResourceBarrierThunk),
				a_barrierPrevious,
				a_barrierAttempted,
				a_barrierWritten);
			const bool clearStateRestored = RestoreVtableSlot(
				a_vtable,
				clearStateIndex,
				FunctionAddress(&ClearStateThunk),
				a_clearStatePrevious,
				a_clearStateAttempted,
				a_clearStateWritten);
			const bool resetRestored = RestoreVtableSlot(
				a_vtable,
				resetIndex,
				FunctionAddress(&ResetThunk),
				a_resetPrevious,
				a_resetAttempted,
				a_resetWritten);
			return heapRestored && barrierRestored && clearStateRestored && resetRestored;
		}

		[[nodiscard]] bool EnsureCommandListHooks() noexcept
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
				drawEnabled.store(false, std::memory_order_release);
				commandListState.store(HookState::Failed, std::memory_order_release);
				return false;
			};

			auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
			if (!renderer || !renderer->GetDevice() || !renderer->GetGraphicsQueue()) {
				commandListState.store(HookState::Uninitialized, std::memory_order_release);
				return false;
			}

			ComPtr<ID3D12Device> device;
			auto*                borrowedDevice = reinterpret_cast<ID3D12Device*>(renderer->GetDevice());
			if (FAILED(borrowedDevice->QueryInterface(IID_PPV_ARGS(device.GetAddressOf())))) {
				logger::critical("Could not acquire the Starfield DirectX 12 device");
				return fail();
			}

			ComPtr<ID3D12CommandQueue> graphicsQueue;
			auto*                      borrowedQueue = reinterpret_cast<ID3D12CommandQueue*>(renderer->GetGraphicsQueue());
			if (FAILED(borrowedQueue->QueryInterface(IID_PPV_ARGS(graphicsQueue.GetAddressOf()))) ||
				graphicsQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
				logger::critical("Could not acquire Starfield's direct graphics queue");
				return fail();
			}

			ComPtr<ID3D12Device> queueDevice;
			if (FAILED(graphicsQueue->GetDevice(IID_PPV_ARGS(queueDevice.GetAddressOf()))) ||
				!HasSameComIdentity(queueDevice.Get(), device.Get())) {
				logger::critical("Starfield's graphics queue and renderer device do not match");
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
				const auto proxyResetTarget = ReadVtableSlot(proxyVtable, resetIndex);
				const auto proxyClearStateTarget = ReadVtableSlot(proxyVtable, clearStateIndex);
				const auto proxyBarrierTarget = ReadVtableSlot(proxyVtable, resourceBarrierIndex);
				const auto proxyHeapTarget = ReadVtableSlot(proxyVtable, setDescriptorHeapsIndex);
				if (!IsAdjacentStreamlineTarget(proxyResetTarget) ||
					!IsAdjacentStreamlineTarget(proxyClearStateTarget) ||
					!IsAdjacentStreamlineTarget(proxyBarrierTarget) ||
					!IsAdjacentStreamlineTarget(proxyHeapTarget)) {
					logger::critical(
						"The Streamline native-interface query came from unsupported command-list targets: "
						"Reset=0x{:X}, ClearState=0x{:X}, ResourceBarrier=0x{:X}, "
						"SetDescriptorHeaps=0x{:X}",
						proxyResetTarget,
						proxyClearStateTarget,
						proxyBarrierTarget,
						proxyHeapTarget);
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
			if (!HasSameComIdentity(hookDevice.Get(), expectedHookDevice)) {
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
			const auto                      resetTarget = ReadVtableSlot(vtable, resetIndex);
			const auto                      clearStateTarget = ReadVtableSlot(vtable, clearStateIndex);
			const auto                      barrierTarget = ReadVtableSlot(vtable, resourceBarrierIndex);
			const auto                      heapTarget = ReadVtableSlot(vtable, setDescriptorHeapsIndex);
			if (!IsNativeD3D12Target(resetTarget) || !IsNativeD3D12Target(clearStateTarget) ||
				!IsNativeD3D12Target(barrierTarget) ||
				!IsNativeD3D12Target(heapTarget) ||
				resetTarget == FunctionAddress(&ResetThunk) ||
				clearStateTarget == FunctionAddress(&ClearStateThunk) ||
				barrierTarget == FunctionAddress(&ResourceBarrierThunk) ||
				heapTarget == FunctionAddress(&SetDescriptorHeapsThunk)) {
				logger::critical(
					"Unsupported native D3D12 command-list targets: "
					"Reset=0x{:X}, ClearState=0x{:X}, ResourceBarrier=0x{:X}, "
					"SetDescriptorHeaps=0x{:X}",
					resetTarget,
					clearStateTarget,
					barrierTarget,
					heapTarget);
				return fail();
			}

			resetOriginal.store(
				reinterpret_cast<ResetFunction>(resetTarget),
				std::memory_order_release);
			clearStateOriginal.store(
				reinterpret_cast<ClearStateFunction>(clearStateTarget),
				std::memory_order_release);
			resourceBarrierOriginal.store(
				reinterpret_cast<ResourceBarrierFunction>(barrierTarget),
				std::memory_order_release);
			setDescriptorHeapsOriginal.store(
				reinterpret_cast<SetDescriptorHeapsFunction>(heapTarget),
				std::memory_order_release);

			std::uintptr_t resetPrevious{};
			std::uintptr_t clearStatePrevious{};
			std::uintptr_t barrierPrevious{};
			std::uintptr_t heapPrevious{};
			bool           resetWritten{ false };
			bool           clearStateWritten{ false };
			bool           barrierWritten{ false };
			bool           heapWritten{ false };
			bool           resetAttempted{ false };
			bool           clearStateAttempted{ false };
			bool           barrierAttempted{ false };
			bool           heapAttempted{ false };
			if (!WriteVtableSlot(
					vtable,
					resetIndex,
					resetTarget,
					FunctionAddress(&ResetThunk),
					resetPrevious,
					resetAttempted,
					resetWritten) ||
				!WriteVtableSlot(
					vtable,
					clearStateIndex,
					clearStateTarget,
					FunctionAddress(&ClearStateThunk),
					clearStatePrevious,
					clearStateAttempted,
					clearStateWritten) ||
				!WriteVtableSlot(
					vtable,
					resourceBarrierIndex,
					barrierTarget,
					FunctionAddress(&ResourceBarrierThunk),
					barrierPrevious,
					barrierAttempted,
					barrierWritten) ||
				!WriteVtableSlot(
					vtable,
					setDescriptorHeapsIndex,
					heapTarget,
					FunctionAddress(&SetDescriptorHeapsThunk),
					heapPrevious,
					heapAttempted,
					heapWritten)) {
				const bool restored = RollBackCommandListHooks(
					vtable,
					heapAttempted,
					heapWritten,
					heapPrevious,
					barrierAttempted,
					barrierWritten,
					barrierPrevious,
					clearStateAttempted,
					clearStateWritten,
					clearStatePrevious,
					resetAttempted,
					resetWritten,
					resetPrevious);
				logger::critical(
					"Failed to commit the D3D12 command-list hooks; rollback {}",
					restored ? "succeeded" : "was incomplete");
				return fail();
			}

			selfTestResetSeen = false;
			selfTestClearStateSeen = false;
			selfTestResourceBarrierSeen = false;
			selfTestDescriptorHeapsSeen = false;
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

			if (!selfTestResetSeen || !selfTestClearStateSeen || !selfTestResourceBarrierSeen ||
				!selfTestDescriptorHeapsSeen || FAILED(closeResult) || FAILED(resetResult) ||
				FAILED(finalCloseResult)) {
				const bool restored = RollBackCommandListHooks(
					vtable,
					heapAttempted,
					heapWritten,
					heapPrevious,
					barrierAttempted,
					barrierWritten,
					barrierPrevious,
					clearStateAttempted,
					clearStateWritten,
					clearStatePrevious,
					resetAttempted,
					resetWritten,
					resetPrevious);
				logger::critical(
					"The D3D12 hook self-test failed; rollback {}",
					restored ? "succeeded" : "was incomplete");
				return fail();
			}

			internalD3D = true;
			const bool rendererReady = D3D12Renderer::Initialize(hookDevice.Get());
			internalD3D = false;
			if (!rendererReady) {
				const bool restored = RollBackCommandListHooks(
					vtable,
					heapAttempted,
					heapWritten,
					heapPrevious,
					barrierAttempted,
					barrierWritten,
					barrierPrevious,
					clearStateAttempted,
					clearStateWritten,
					clearStatePrevious,
					resetAttempted,
					resetWritten,
					resetPrevious);
				logger::critical(
					"The ImGui renderer could not be initialized; D3D12 rollback {}",
					restored ? "succeeded" : "was incomplete");
				return fail();
			}

			commandListState.store(HookState::Ready, std::memory_order_release);
			drawEnabled.store(true, std::memory_order_release);
			logger::info("D3D12 command-list hooks installed and self-tested");
			return true;
		}

		void BeginThunk(
			RE::CreationRendererPrivate::RenderPass*              a_pass,
			RE::CreationRendererPrivate::RenderPassContext*       a_context,
			RE::CreationRendererPrivate::RenderPassExecutionData* a_executionData) noexcept
		{
			beginPassHits.fetch_add(1, std::memory_order_relaxed);
			ResetRegion();
			if (scaleformState.load(std::memory_order_acquire) == HookState::Ready) {
				static_cast<void>(EnsureCommandListHooks());
			}

			if (const auto original = beginOriginal.load(std::memory_order_acquire)) {
				original(a_pass, a_context, a_executionData);
			}

			if (commandListState.load(std::memory_order_acquire) == HookState::Ready) {
				MaybeLogRenderProbe();
			}
		}

		void EndThunk(
			RE::CreationRendererPrivate::RenderPass*              a_pass,
			RE::CreationRendererPrivate::RenderPassContext*       a_context,
			RE::CreationRendererPrivate::RenderPassExecutionData* a_executionData) noexcept
		{
			endPassHits.fetch_add(1, std::memory_order_relaxed);
			if (const auto original = endOriginal.load(std::memory_order_acquire)) {
				original(a_pass, a_context, a_executionData);
			}

			regionState = {};
			regionState.Active = true;
			regionState.Previous = previousRegion.load(std::memory_order_acquire);
		}

		void CompositeThunk(
			RE::CreationRendererPrivate::RenderPass*              a_pass,
			RE::CreationRendererPrivate::RenderPassContext*       a_context,
			RE::CreationRendererPrivate::RenderPassExecutionData* a_executionData) noexcept
		{
			compositePassHits.fetch_add(1, std::memory_order_relaxed);
			if (regionState.Active) {
				completedRegions.fetch_add(1, std::memory_order_relaxed);
				if (regionState.SawValidCandidate) {
					previousRegion.store(
						regionState.SawCopyTarget ? PreviousRegion::FrameGeneration : PreviousRegion::Normal,
						std::memory_order_release);
				}
			}
			ResetRegion();

			if (const auto original = compositeOriginal.load(std::memory_order_acquire)) {
				original(a_pass, a_context, a_executionData);
			}
		}

		[[nodiscard]] bool InstallScaleformHooks()
		{
			using namespace RE::CreationRendererPrivate;
			constexpr auto slot = kExecuteRenderPassVTableIndex;

			REL::Relocation<std::uintptr_t>      beginVtable{ ScaleformRenderPass::Begin::VTABLE[0] };
			REL::Relocation<std::uintptr_t>      endVtable{ ScaleformRenderPass::End::VTABLE[0] };
			REL::Relocation<std::uintptr_t>      compositeVtable{ ScaleformRenderPass::Composite::VTABLE[0] };
			REL::Relocation<ExecuteRenderPass_t> expectedBegin{ ScaleformRenderPass::Begin::Execute };
			REL::Relocation<ExecuteRenderPass_t> expectedEnd{ ScaleformRenderPass::End::Execute };
			REL::Relocation<ExecuteRenderPass_t> expectedComposite{ ScaleformRenderPass::Composite::Execute };

			const auto beginTarget = expectedBegin.address();
			const auto endTarget = expectedEnd.address();
			const auto compositeTarget = expectedComposite.address();
			if (!IsExecutableAddress(beginTarget) || !IsExecutableAddress(endTarget) ||
				!IsExecutableAddress(compositeTarget) ||
				ReadVtableSlot(beginVtable, slot) != beginTarget ||
				ReadVtableSlot(endVtable, slot) != endTarget ||
				ReadVtableSlot(compositeVtable, slot) != compositeTarget) {
				logger::critical("The Scaleform render-pass seam does not match Starfield 1.16.244");
				return false;
			}

			beginOriginal.store(reinterpret_cast<RenderPassFunction>(beginTarget), std::memory_order_release);
			endOriginal.store(reinterpret_cast<RenderPassFunction>(endTarget), std::memory_order_release);
			compositeOriginal.store(
				reinterpret_cast<RenderPassFunction>(compositeTarget),
				std::memory_order_release);

			std::uintptr_t beginPrevious{};
			std::uintptr_t endPrevious{};
			std::uintptr_t compositePrevious{};
			bool           beginWritten{ false };
			bool           endWritten{ false };
			bool           compositeWritten{ false };
			bool           beginAttempted{ false };
			bool           endAttempted{ false };
			bool           compositeAttempted{ false };

			const bool committed =
				WriteVtableSlot(
					beginVtable,
					slot,
					beginTarget,
					FunctionAddress(&BeginThunk),
					beginPrevious,
					beginAttempted,
					beginWritten) &&
				WriteVtableSlot(
					endVtable,
					slot,
					endTarget,
					FunctionAddress(&EndThunk),
					endPrevious,
					endAttempted,
					endWritten) &&
				WriteVtableSlot(
					compositeVtable,
					slot,
					compositeTarget,
					FunctionAddress(&CompositeThunk),
					compositePrevious,
					compositeAttempted,
					compositeWritten);

			if (!committed) {
				const bool compositeRestored = RestoreVtableSlot(
					compositeVtable,
					slot,
					FunctionAddress(&CompositeThunk),
					compositePrevious,
					compositeAttempted,
					compositeWritten);
				const bool endRestored = RestoreVtableSlot(
					endVtable,
					slot,
					FunctionAddress(&EndThunk),
					endPrevious,
					endAttempted,
					endWritten);
				const bool beginRestored = RestoreVtableSlot(
					beginVtable,
					slot,
					FunctionAddress(&BeginThunk),
					beginPrevious,
					beginAttempted,
					beginWritten);
				const bool restored = compositeRestored && endRestored && beginRestored;
				logger::critical(
					"Failed to commit the Scaleform render-pass hooks; rollback {}",
					restored ? "succeeded" : "was incomplete, so the DLL must remain loaded");
				if (!restored) {
					scaleformState.store(HookState::Failed, std::memory_order_release);
				}
				return false;
			}

			return true;
		}
	}

	bool Install()
	{
		HookState expected = HookState::Uninitialized;
		if (!scaleformState.compare_exchange_strong(
				expected,
				HookState::Installing,
				std::memory_order_acq_rel)) {
			return expected == HookState::Ready;
		}

		if (!InstallScaleformHooks()) {
			scaleformState.store(HookState::Failed, std::memory_order_release);
			return false;
		}

		scaleformState.store(HookState::Ready, std::memory_order_release);
		logger::info("Scaleform render-pass hooks installed");
		return true;
	}
}
