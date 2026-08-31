#pragma once

#include <cstdint>

#include <Windows.h>

namespace SFSEMenuFramework::Win32Platform
{
	using HostWindowCallback = void (*)() noexcept;

	enum class InitializeResult : std::uint8_t
	{
		Ready,
		Deferred,
		Failed
	};

	[[nodiscard]] InitializeResult Initialize();
	[[nodiscard]] bool             IsCurrentThreadHostWindowThread();
	[[nodiscard]] bool             IsInitialized() noexcept;
	[[nodiscard]] bool             HasLiveBackend() noexcept;
	[[nodiscard]] bool             IsHostWindowUsable() noexcept;
	[[nodiscard]] bool             UpdateInputState(
		bool a_acceptInput,
		std::uint64_t a_earlyRawMouseGeneration = 0);
	[[nodiscard]] bool             PrepareFrame();
	void                           SetHostWindowCallback(HostWindowCallback a_callback) noexcept;
	[[nodiscard]] bool             PostHostWindowCallback() noexcept;
}
