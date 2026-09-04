#include "runtime/ConsumerValidation.h"

#include <Windows.h>

namespace SFSEMenuFramework::Detail
{
	bool IsExecutableImageAddress(
		const void* a_address,
		void**      a_ownerModule) noexcept
	{
		MEMORY_BASIC_INFORMATION information{};
		if (::VirtualQuery(a_address, &information, sizeof(information)) !=
				sizeof(information) ||
			information.State != MEM_COMMIT ||
			information.Type != MEM_IMAGE ||
			(information.Protect & PAGE_GUARD) != 0 ||
			!information.AllocationBase) {
			return false;
		}
		const auto protection = information.Protect & 0xFF;
		const bool executable = protection == PAGE_EXECUTE ||
			protection == PAGE_EXECUTE_READ ||
			protection == PAGE_EXECUTE_READWRITE ||
			protection == PAGE_EXECUTE_WRITECOPY;
		if (executable && a_ownerModule) {
			*a_ownerModule = information.AllocationBase;
		}
		return executable;
	}
}
