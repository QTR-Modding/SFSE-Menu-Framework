#pragma once

#include <SFSEMenuFramework/API.h>

#include <atomic>
#include <cstdint>

namespace SFSEMenuFramework
{
	class WindowInterface final
	{
	public:
		std::atomic<bool>          IsOpen{ false };
		std::atomic<bool>          BlockUserInput{ true };
		std::atomic<std::uint64_t> OpenGeneration{ 0 };
	};

	using WindowRenderFunction = void(__stdcall*)(const Model::RenderContext&);

	class WindowManager final
	{
	public:
		static WindowInterface* AddWindow(WindowRenderFunction a_renderFunction);
		[[nodiscard]] static std::uint64_t RenderOpenWindows(
			const Model::RenderContext& a_context);

		static bool                          SetMainWindow(WindowInterface* a_window) noexcept;
		[[nodiscard]] static WindowInterface* GetMainWindow() noexcept;
		[[nodiscard]] static bool SetMainWindowOpen(bool a_open) noexcept;
		[[nodiscard]] static bool ToggleMainWindow() noexcept;
		[[nodiscard]] static std::uint64_t GetMainWindowOpenGeneration() noexcept;
		[[nodiscard]] static bool IsMainWindowOpenGeneration(std::uint64_t a_generation) noexcept;
	};
}
