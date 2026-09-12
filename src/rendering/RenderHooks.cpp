#include "rendering/RenderHooks.h"
#include "rendering/RenderHooksInternal.h"
#include "rendering/StreamlineDiagnostic.h"

#include "rendering/D3D12Renderer.h"

#include <RE/C/CreationRenderer.h>

#include <Windows.h>

#include <array>
#include <atomic>
#include <span>

namespace SFSEMenuFramework::RenderHooks
{
	namespace
	{
		using RenderPassFunction = void* (*)(void*, void*, void*, void*);

		enum class HookState : std::uint8_t
		{
			Uninitialized,
			Installing,
			Ready,
			Failed
		};

		std::atomic<HookState> scaleformState{ HookState::Uninitialized };
		std::atomic<RenderPassFunction> beginOriginal{ nullptr };
		std::atomic<RenderPassFunction> endOriginal{ nullptr };
		std::atomic<RenderPassFunction> compositeOriginal{ nullptr };
		std::atomic<bool> postCompositeHandoff{ false };

		void* BeginThunk(void*, void*, void*, void*) noexcept;
		void* EndThunk(void*, void*, void*, void*) noexcept;
		void* CompositeThunk(void*, void*, void*, void*) noexcept;
	}

	bool Detail::HasMemoryAccess(
			std::uintptr_t a_address,
			bool           a_executable,
			std::size_t    a_size)
	{
		if (!a_address) {
			return false;
		}
		MEMORY_BASIC_INFORMATION memory{};
		if (::VirtualQuery(reinterpret_cast<const void*>(a_address), &memory, sizeof(memory)) == 0 ||
			memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0) {
			return false;
		}
		const auto regionEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
		if (a_address > regionEnd || a_size > regionEnd - a_address) {
			return false;
		}
		if (!a_executable) {
			return (memory.Protect & 0xFF) != PAGE_NOACCESS;
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

	std::uintptr_t Detail::ReadVtableSlot(
		const REL::Relocation<std::uintptr_t>& a_vtable,
		std::size_t                            a_index)
	{
		const auto address = a_vtable.address() + sizeof(std::uintptr_t) * a_index;
		if (address % alignof(std::uintptr_t) != 0 ||
			!HasMemoryAccess(address, false, sizeof(std::uintptr_t))) {
			return 0;
		}
		return *reinterpret_cast<const std::uintptr_t*>(address);
	}

	bool Detail::VtableHook::Commit()
	{
		if (ReadVtableSlot(*Vtable, Index) != Expected) {
			return false;
		}
		Attempted = true;
		Previous = Vtable->write_vfunc(Index, Replacement);
		return Previous == Expected &&
			ReadVtableSlot(*Vtable, Index) == Replacement;
	}

	bool Detail::VtableHook::RollBack()
	{
		if (!Attempted) {
			return true;
		}
		const auto current = ReadVtableSlot(*Vtable, Index);
		if (current == Previous) {
			return true;
		}
		if (current != Replacement) {
			return false;
		}
		Vtable->write_vfunc(Index, Previous);
		return ReadVtableSlot(*Vtable, Index) == Previous;
	}

	bool Detail::CommitHooks(std::span<VtableHook> a_hooks)
	{
		for (auto& hook : a_hooks) {
			if (!hook.Commit()) {
				return false;
			}
		}
		return true;
	}

	bool Detail::RollBackHooks(std::span<VtableHook> a_hooks)
	{
		bool restored = true;
		for (auto hook = a_hooks.rbegin(); hook != a_hooks.rend(); ++hook) {
			restored = hook->RollBack() && restored;
		}
		return restored;
	}

	namespace
	{
		using namespace Detail;

		void* BeginThunk(void* a_pass, void* a_context, void* a_executionData, void* a_r9) noexcept
		{
			if (postCompositeHandoff.load(std::memory_order_acquire)) {
				Detail::FinalizeRegionBeforeComposite();
			} else {
				Detail::ResetRegion();
			}
			StreamlineDiagnostic::ResetRegion();
			if (scaleformState.load(std::memory_order_acquire) == HookState::Ready) {
				if (!StreamlineDiagnostic::EnsureInstalled()) {
					static_cast<void>(Detail::EnsureCommandListHooks());
				}
			}

			return beginOriginal.load(std::memory_order_acquire)(
				a_pass, a_context, a_executionData, a_r9);
		}

		void* EndThunk(void* a_pass, void* a_context, void* a_executionData, void* a_r9) noexcept
		{
			const auto result = endOriginal.load(std::memory_order_acquire)(
				a_pass, a_context, a_executionData, a_r9);
			if (!postCompositeHandoff.load(std::memory_order_acquire)) {
				Detail::ActivateRegionAfterScaleformEnd();
				StreamlineDiagnostic::ActivateRegion();
			}
			return result;
		}

		void* CompositeThunk(void* a_pass, void* a_context, void* a_executionData, void* a_r9) noexcept
		{
			const bool postComposite = postCompositeHandoff.load(std::memory_order_acquire);
			if (!postComposite) {
				Detail::FinalizeRegionBeforeComposite();
				StreamlineDiagnostic::ResetRegion();
			}

			const auto result = compositeOriginal.load(std::memory_order_acquire)(
				a_pass, a_context, a_executionData, a_r9);
			if (postComposite) {
				Detail::ActivateRegionAfterScaleformEnd();
				StreamlineDiagnostic::ActivateRegion();
			}
			return result;
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
			const auto beginCurrent = ReadVtableSlot(beginVtable, slot);
			const auto endCurrent = ReadVtableSlot(endVtable, slot);
			const auto compositeCurrent = ReadVtableSlot(compositeVtable, slot);
			const auto beginReplacement = FunctionAddress(&BeginThunk);
			const auto endReplacement = FunctionAddress(&EndThunk);
			const auto compositeReplacement = FunctionAddress(&CompositeThunk);
			if (!HasMemoryAccess(beginTarget, true) || !HasMemoryAccess(endTarget, true) ||
				!HasMemoryAccess(compositeTarget, true) || !HasMemoryAccess(beginCurrent, true) ||
				!HasMemoryAccess(endCurrent, true) || !HasMemoryAccess(compositeCurrent, true) ||
				beginCurrent == beginReplacement || endCurrent == endReplacement ||
				compositeCurrent == compositeReplacement) {
				logger::critical("The Scaleform render-pass hooks are unavailable");
				return false;
			}
			if (beginCurrent != beginTarget || endCurrent != endTarget ||
				compositeCurrent != compositeTarget) {
				logger::info("Existing Scaleform render-pass hooks detected; chaining");
			}

			const bool usePostComposite = ::GetModuleHandleW(L"Luma.dll") != nullptr;
			postCompositeHandoff.store(usePostComposite, std::memory_order_release);
			D3D12Renderer::ConfigureRenderTargetFormat(
				usePostComposite ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM);
			if (usePostComposite) {
				logger::info("Luma detected; using the RGBA16F post-composite handoff");
			}

			beginOriginal.store(reinterpret_cast<RenderPassFunction>(beginCurrent), std::memory_order_release);
			endOriginal.store(reinterpret_cast<RenderPassFunction>(endCurrent), std::memory_order_release);
			compositeOriginal.store(
				reinterpret_cast<RenderPassFunction>(compositeCurrent),
				std::memory_order_release);

			std::array<VtableHook, 3> hooks{
				VtableHook{ &beginVtable, slot, beginCurrent, beginReplacement },
				VtableHook{ &endVtable, slot, endCurrent, endReplacement },
				VtableHook{ &compositeVtable, slot, compositeCurrent, compositeReplacement }
			};
			if (!CommitHooks(hooks)) {
				const bool restored = RollBackHooks(hooks);
				logger::critical(
					"Failed to commit the Scaleform render-pass hooks; rollback {}",
					restored ? "succeeded" : "was incomplete, so the DLL must remain loaded");
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
