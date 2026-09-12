#include "rendering/CommandListState.h"
#include "rendering/RenderHooksInternal.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace SFSEMenuFramework::CommandListState
{
	namespace
	{
		using List = ID3D12GraphicsCommandList;
		using namespace RenderHooks::Detail;
		struct Registry
		{
			std::mutex Mutex;
			std::unordered_map<List*, std::unique_ptr<Snapshot>> Lists;
			void** Vtable{};
			bool Installed{};
			std::size_t CapacityRejectedResets{};
		};

		Registry& States()
		{
			static auto* states = new Registry;
			return *states;
		}
		thread_local bool injecting{};

		void Pipeline(Snapshot& s, ID3D12PipelineState* p) { s.Pipeline = p; }
		void GraphicsRoot(Snapshot& s, ID3D12RootSignature* p)
		{
			if (s.GraphicsRoot.Get() != p) { s.Graphics = {}; }
			s.GraphicsRoot = p;
		}
		void ComputeRoot(Snapshot& s, ID3D12RootSignature* p)
		{
			if (s.ComputeRoot.Get() != p) { s.ComputeTableSet.reset(); }
			s.ComputeRoot = p;
		}
		void Heaps(Snapshot& s, UINT n, ID3D12DescriptorHeap* const* heaps)
		{
			if (n > s.Heaps.size() || (n && !heaps)) { s.Complete = false; return; }
			bool changed = n != s.HeapCount;
			for (UINT i = 0; i < n; ++i) { changed |= s.Heaps[i].Get() != heaps[i]; }
			if (changed) {
				for (auto& arg : s.Graphics) {
					if (arg.Type == RootArgument::Kind::Table) { arg = {}; }
				}
				s.ComputeTableSet.reset();
			}
			s.Heaps = {};
			s.HeapCount = n;
			for (UINT i = 0; i < n; ++i) { s.Heaps[i] = heaps[i]; }
		}
		void Table(Snapshot& s, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h)
		{
			if (i >= s.Graphics.size()) { s.Complete = false; return; }
			s.Graphics[i].Type = RootArgument::Kind::Table;
			s.Graphics[i].Value = h.ptr;
		}
		void ComputeTable(Snapshot& s, UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h)
		{
			if (i >= s.ComputeTables.size()) { s.Complete = false; return; }
			s.ComputeTables[i] = h;
			s.ComputeTableSet.set(i);
		}
		void Constants(Snapshot& s, UINT i, UINT n, const void* data, UINT offset)
		{
			if (i >= s.Graphics.size() || offset > 64 || n > 64 - offset || (n && !data)) {
				s.Complete = false; return;
			}
			auto& arg = s.Graphics[i];
			arg.Type = RootArgument::Kind::Constants;
			for (UINT j = 0; j < n; ++j) {
				arg.Constants[offset + j] = static_cast<const UINT*>(data)[j];
				arg.Written.set(offset + j);
			}
		}
		void Constant(Snapshot& s, UINT i, UINT data, UINT offset) { Constants(s, i, 1, &data, offset); }
		template<RootArgument::Kind Kind>
		void Address(Snapshot& s, UINT i, D3D12_GPU_VIRTUAL_ADDRESS address)
		{
			if (i >= s.Graphics.size()) { s.Complete = false; return; }
			s.Graphics[i].Type = Kind;
			s.Graphics[i].Value = address;
		}
		void Viewports(Snapshot& s, UINT n, const D3D12_VIEWPORT* p)
		{
			if (n > s.Viewports.size() || (n && !p)) { s.Complete = false; return; }
			s.ViewportCount = n;
			if (n) { std::copy_n(p, n, s.Viewports.begin()); }
		}
		void Scissors(Snapshot& s, UINT n, const D3D12_RECT* p)
		{
			if (n > s.Scissors.size() || (n && !p)) { s.Complete = false; return; }
			s.ScissorCount = n;
			if (n) { std::copy_n(p, n, s.Scissors.begin()); }
		}
		void Blend(Snapshot& s, const FLOAT* p)
		{
			if (p) { std::copy_n(p, 4, s.Blend.begin()); }
			else { s.Blend = { 1, 1, 1, 1 }; }
		}
		void Topology(Snapshot& s, D3D12_PRIMITIVE_TOPOLOGY t) { s.Topology = t; }
		void Vertex(Snapshot& s, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* p)
		{
			if (start == 0 && n) { s.VertexBuffer = p ? p[0] : D3D12_VERTEX_BUFFER_VIEW{}; }
		}
		void Index(Snapshot& s, const D3D12_INDEX_BUFFER_VIEW* p)
		{
			s.HasIndexBuffer = p != nullptr;
			s.IndexBuffer = p ? *p : D3D12_INDEX_BUFFER_VIEW{};
		}
		void Targets(Snapshot& s, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* p,
			BOOL contiguous, const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
		{
			if (n > s.RenderTargets.size() || (n && !p)) { s.Complete = false; return; }
			s.RenderTargetCount = n;
			s.ContiguousTargets = contiguous;
			if (n) { std::copy_n(p, contiguous ? 1 : n, s.RenderTargets.begin()); }
			s.HasDepthTarget = depth != nullptr;
			s.DepthTarget = depth ? *depth : D3D12_CPU_DESCRIPTOR_HANDLE{};
		}
		void Bundle(Snapshot& s, List*) { s.Complete = false; s.InvalidReason = "ExecuteBundle"; }
		void Indirect(Snapshot& s, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
			ID3D12Resource*, UINT64) { s.Complete = false; s.InvalidReason = "ExecuteIndirect"; }
		void RenderPass(Snapshot& s, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*,
			const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS)
		{ s.Complete = false; s.InvalidReason = "BeginRenderPass"; }
		void RaytracingPipeline(Snapshot& s, ID3D12StateObject*)
		{ s.Complete = false; s.InvalidReason = "SetPipelineState1"; }

		// Typed forwarding preserves each SDK method's calling convention and arguments.
		template<std::size_t Slot, auto Observe> struct Hook;
		template<std::size_t Slot, class... Args, void (*Observe)(Snapshot&, Args...)>
		struct Hook<Slot, Observe>
		{
			using Function = void(STDMETHODCALLTYPE*)(List*, Args...);
			inline static std::atomic<Function> Original{};
			static void STDMETHODCALLTYPE Call(List* list, Args... args)
			{
				Original.load(std::memory_order_acquire)(list, args...);
				if (injecting) { return; }
				auto& registry = States();
				std::scoped_lock lock{ registry.Mutex };
				const auto found = registry.Lists.find(list);
				if (found != registry.Lists.end()) { Observe(*found->second, args...); }
			}
			static VtableHook Prepare(REL::Relocation<std::uintptr_t>& table)
			{
				const auto original = ReadVtableSlot(table, Slot);
				Original.store(reinterpret_cast<Function>(original), std::memory_order_release);
				return { &table, Slot, original, FunctionAddress(&Call) };
			}
		};

		using ResetFunction = HRESULT(STDMETHODCALLTYPE*)(List*, ID3D12CommandAllocator*, ID3D12PipelineState*);
		using ClearFunction = void(STDMETHODCALLTYPE*)(List*, ID3D12PipelineState*);
		std::atomic<ResetFunction> originalReset{};
		std::atomic<ClearFunction> originalClear{};
		void ResetSnapshot(List* list, ID3D12PipelineState* pipeline)
		{
			if (injecting) { return; }
			auto& registry = States();
			std::scoped_lock lock{ registry.Mutex };
			auto it = registry.Lists.find(list);
			if (it == registry.Lists.end()) {
				// Bound retained state. Unknown lists fall back rather than evicting live state.
				if (registry.Lists.size() >= 128) {
					if (++registry.CapacityRejectedResets == 1) {
						logger::warn("Command-list tracker reached 128 entries; rejecting further new-list resets");
					}
					return;
				}
				it = registry.Lists.emplace(list, std::make_unique<Snapshot>()).first;
			}
			const auto epoch = it->second->Epoch + 1;
			*it->second = {};
			it->second->Epoch = epoch;
			it->second->Complete = true;
			it->second->Pipeline = pipeline;
		}
		HRESULT STDMETHODCALLTYPE Reset(List* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pipeline)
		{
			const auto result = originalReset.load(std::memory_order_acquire)(list, allocator, pipeline);
			if (SUCCEEDED(result)) { ResetSnapshot(list, pipeline); }
			return result;
		}
		void STDMETHODCALLTYPE Clear(List* list, ID3D12PipelineState* pipeline)
		{
			originalClear.load(std::memory_order_acquire)(list, pipeline);
			ResetSnapshot(list, pipeline);
		}
	}

	bool Snapshot::Ready() const noexcept
	{
		return Complete && Pipeline && GraphicsRoot && HeapCount && ViewportCount && ScissorCount;
	}

	void Snapshot::Restore(List* list) const
	{
		std::array<ID3D12DescriptorHeap*, 2> heaps{};
		for (UINT i = 0; i < HeapCount; ++i) { heaps[i] = Heaps[i].Get(); }
		list->SetDescriptorHeaps(HeapCount, heaps.data());
		list->SetPipelineState(Pipeline.Get());
		list->SetGraphicsRootSignature(GraphicsRoot.Get());
		for (UINT i = 0; i < Graphics.size(); ++i) {
			const auto& arg = Graphics[i];
			switch (arg.Type) {
			case RootArgument::Kind::Unset: break;
			case RootArgument::Kind::Table: list->SetGraphicsRootDescriptorTable(i, { arg.Value }); break;
			case RootArgument::Kind::CBV: list->SetGraphicsRootConstantBufferView(i, arg.Value); break;
			case RootArgument::Kind::SRV: list->SetGraphicsRootShaderResourceView(i, arg.Value); break;
			case RootArgument::Kind::UAV: list->SetGraphicsRootUnorderedAccessView(i, arg.Value); break;
			case RootArgument::Kind::Constants:
				for (UINT j = 0; j < arg.Constants.size(); ++j) {
					if (arg.Written[j]) { list->SetGraphicsRoot32BitConstant(i, arg.Constants[j], j); }
				}
				break;
			}
		}
		for (UINT i = 0; i < ComputeTables.size(); ++i) {
			if (ComputeTableSet[i]) { list->SetComputeRootDescriptorTable(i, ComputeTables[i]); }
		}
		list->RSSetViewports(ViewportCount, Viewports.data());
		list->RSSetScissorRects(ScissorCount, Scissors.data());
		list->OMSetBlendFactor(Blend.data());
		list->IASetPrimitiveTopology(Topology);
		list->IASetVertexBuffers(0, 1, &VertexBuffer);
		list->IASetIndexBuffer(HasIndexBuffer ? &IndexBuffer : nullptr);
		list->OMSetRenderTargets(RenderTargetCount, RenderTargets.data(), ContiguousTargets,
			HasDepthTarget ? &DepthTarget : nullptr);
	}

	bool Install(List* list)
	{
		if (!list) { return false; }
		auto& registry = States();
		std::scoped_lock lock{ registry.Mutex };
		auto** vtable = *reinterpret_cast<void***>(list);
		if (registry.Vtable) { return registry.Installed && registry.Vtable == vtable; }
		registry.Vtable = vtable;
		REL::Relocation<std::uintptr_t> table{ reinterpret_cast<std::uintptr_t>(vtable) };
		const auto reset = ReadVtableSlot(table, 10);
		const auto clear = ReadVtableSlot(table, 11);
		originalReset.store(reinterpret_cast<ResetFunction>(reset), std::memory_order_release);
		originalClear.store(reinterpret_cast<ClearFunction>(clear), std::memory_order_release);
		std::vector hooks{
			VtableHook{ &table, 10, reset, FunctionAddress(&Reset) },
			VtableHook{ &table, 11, clear, FunctionAddress(&Clear) },
			Hook<20, Topology>::Prepare(table), Hook<21, Viewports>::Prepare(table),
			Hook<22, Scissors>::Prepare(table), Hook<23, Blend>::Prepare(table),
			Hook<25, Pipeline>::Prepare(table), Hook<27, Bundle>::Prepare(table),
			Hook<28, Heaps>::Prepare(table), Hook<29, ComputeRoot>::Prepare(table),
			Hook<30, GraphicsRoot>::Prepare(table), Hook<31, ComputeTable>::Prepare(table),
			Hook<32, Table>::Prepare(table), Hook<34, Constant>::Prepare(table),
			Hook<36, Constants>::Prepare(table),
			Hook<38, Address<RootArgument::Kind::CBV>>::Prepare(table),
			Hook<40, Address<RootArgument::Kind::SRV>>::Prepare(table),
			Hook<42, Address<RootArgument::Kind::UAV>>::Prepare(table),
			Hook<43, Index>::Prepare(table), Hook<44, Vertex>::Prepare(table),
			Hook<46, Targets>::Prepare(table), Hook<59, Indirect>::Prepare(table)
		};
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> list4;
		if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4)))) {
			// Extended interfaces must use the same tracked implementation.
			if (*reinterpret_cast<void***>(list4.Get()) != vtable) { return false; }
			hooks.push_back(Hook<68, RenderPass>::Prepare(table));
			hooks.push_back(Hook<75, RaytracingPipeline>::Prepare(table));
		}
		for (const auto& hook : hooks) {
			if (!HasMemoryAccess(hook.Expected, true)) { return false; }
		}
		registry.Installed = CommitHooks(hooks);
		if (!registry.Installed) {
			const bool restored = RollBackHooks(hooks);
			logger::error("Command-list state hook installation failed; rollback {}", restored);
		}
		return registry.Installed;
	}

	bool Capture(List* list, Snapshot& snapshot, const char** reason)
	{
		auto& registry = States();
		std::scoped_lock lock{ registry.Mutex };
		const auto found = registry.Lists.find(list);
		if (!registry.Installed || found == registry.Lists.end()) {
			if (reason) {
				*reason = registry.CapacityRejectedResets ? "tracker capacity reached (128)" : "Reset not observed";
			}
			return false;
		}
		const auto& state = *found->second;
		if (!state.Ready()) {
			if (reason) {
				*reason = !state.Complete ? state.InvalidReason :
					!state.Pipeline ? "pipeline missing" : !state.GraphicsRoot ? "root missing" :
					!state.HeapCount ? "heaps missing" : "viewport/scissor missing";
			}
			return false;
		}
		snapshot = *found->second;
		return true;
	}

	InjectionScope::InjectionScope() : previous(injecting) { injecting = true; }
	InjectionScope::~InjectionScope() { injecting = previous; }
	std::mutex& RoutingMutex()
	{
		static auto* mutex = new std::mutex;
		return *mutex;
	}
}
