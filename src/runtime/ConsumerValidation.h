#pragma once

#include <SFSEMenuFramework/API.h>

#include <bit>

namespace SFSEMenuFramework::Detail
{
	[[nodiscard]] bool HasMatchingImGuiLayout(
		const Model::ImGuiLayout& a_layout) noexcept;
	[[nodiscard]] bool IsExecutableImageAddress(
		const void* a_address, void** a_ownerModule = nullptr) noexcept;

	template <class Function>
	[[nodiscard]] bool IsExecutableImageFunction(
		Function a_function, void** a_ownerModule = nullptr) noexcept
	{
		static_assert(sizeof(a_function) == sizeof(const void*));
		return a_function && IsExecutableImageAddress(
			std::bit_cast<const void*>(a_function), a_ownerModule);
	}
}
