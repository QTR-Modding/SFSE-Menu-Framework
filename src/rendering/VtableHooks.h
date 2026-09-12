#pragma once

#include <REL/Relocation.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace SFSEMenuFramework::VtableHooks
{
	template <class Function>
	[[nodiscard]] std::uintptr_t FunctionAddress(Function a_function)
	{
		return reinterpret_cast<std::uintptr_t>(a_function);
	}

	[[nodiscard]] bool HasMemoryAccess(
		std::uintptr_t a_address, bool a_executable,
		std::size_t a_size = 1);
	[[nodiscard]] std::uintptr_t ReadVtableSlot(
		const REL::Relocation<std::uintptr_t>& a_vtable,
		std::size_t                            a_index);

	struct VtableHook final
	{
		REL::Relocation<std::uintptr_t>* Vtable;
		std::size_t                      Index;
		std::uintptr_t                   Expected;
		std::uintptr_t                   Replacement;
		std::uintptr_t                   Previous{};
		bool                             Attempted{};

		[[nodiscard]] bool Commit();
		[[nodiscard]] bool RollBack();
	};

	[[nodiscard]] bool CommitHooks(std::span<VtableHook> a_hooks);
	[[nodiscard]] bool RollBackHooks(std::span<VtableHook> a_hooks);

}
