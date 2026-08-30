#pragma once

#include <atomic>
#include <cstdint>

namespace SFSEMenuFramework::Model
{
	using RenderFunction = void(__stdcall*)();

	class WindowInterface final
	{
	public:
		std::atomic<bool> IsOpen{ false };
		std::atomic<bool> BlockUserInput{ true };
		std::atomic<std::uint64_t> OpenGeneration{ 0 };
	};
}
