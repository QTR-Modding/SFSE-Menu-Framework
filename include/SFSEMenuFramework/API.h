#pragma once

#include <atomic>

namespace SFSEMenuFramework::Model
{
	using RenderFunction = void(__stdcall*)();

	class WindowInterface final
	{
	public:
		std::atomic<bool> IsOpen{ false };
		std::atomic<bool> BlockUserInput{ true };
	};
}
