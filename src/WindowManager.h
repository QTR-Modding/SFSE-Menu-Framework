#pragma once

#include <SFSEMenuFramework/API.h>

#include <cstdint>

namespace SFSEMenuFramework
{
	class WindowManager final
	{
	public:
		static Model::WindowInterface* AddWindow(Model::RenderFunction a_renderFunction);
		[[nodiscard]] static std::uint64_t RenderOpenWindows();

		static bool                                  SetMainWindow(Model::WindowInterface* a_window) noexcept;
		[[nodiscard]] static Model::WindowInterface* GetMainWindow() noexcept;
		[[nodiscard]] static bool SetMainWindowOpen(bool a_open) noexcept;
		[[nodiscard]] static bool ToggleMainWindow() noexcept;
		[[nodiscard]] static std::uint64_t GetMainWindowOpenGeneration() noexcept;
		[[nodiscard]] static bool IsMainWindowOpenGeneration(std::uint64_t a_generation) noexcept;
	};
}
