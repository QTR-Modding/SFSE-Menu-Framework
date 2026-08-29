#pragma once

#include <SFSEMenuFramework/API.h>

namespace SFSEMenuFramework
{
	class WindowManager final
	{
	public:
		static Model::WindowInterface* AddWindow(Model::RenderFunction a_renderFunction);
		static void                    RenderOpenWindows();

		static bool                                  SetMainWindow(Model::WindowInterface* a_window) noexcept;
		[[nodiscard]] static Model::WindowInterface* GetMainWindow() noexcept;
	};
}
