#include "rendering/VtableHooks.h"

#include <Windows.h>

namespace SFSEMenuFramework::VtableHooks
{
	bool HasMemoryAccess(
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

	std::uintptr_t ReadVtableSlot(
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

	bool VtableHook::Commit()
	{
		if (ReadVtableSlot(*Vtable, Index) != Expected) {
			return false;
		}
		Attempted = true;
		Previous = Vtable->write_vfunc(Index, Replacement);
		return Previous == Expected &&
			ReadVtableSlot(*Vtable, Index) == Replacement;
	}

	bool VtableHook::RollBack()
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

	bool CommitHooks(std::span<VtableHook> a_hooks)
	{
		for (auto& hook : a_hooks) {
			if (!hook.Commit()) {
				return false;
			}
		}
		return true;
	}

	bool RollBackHooks(std::span<VtableHook> a_hooks)
	{
		bool restored = true;
		for (auto hook = a_hooks.rbegin(); hook != a_hooks.rend(); ++hook) {
			restored = hook->RollBack() && restored;
		}
		return restored;
	}

}
