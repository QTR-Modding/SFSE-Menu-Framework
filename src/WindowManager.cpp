#include "WindowManager.h"

#include <memory>
#include <vector>

namespace
{
	struct Window final
	{
		SFSEMenuFramework::Model::WindowInterface Interface;
		SFSEMenuFramework::Model::RenderFunction  Render{ nullptr };
	};

	std::vector<std::unique_ptr<Window>>       windows;
	SFSEMenuFramework::Model::WindowInterface* mainWindow{ nullptr };
}

SFSEMenuFramework::Model::WindowInterface* SFSEMenuFramework::WindowManager::AddWindow(
	Model::RenderFunction a_renderFunction)
{
	if (!a_renderFunction) {
		return nullptr;
	}

	auto window = std::make_unique<Window>();
	window->Render = a_renderFunction;

	auto* interface = &window->Interface;
	windows.emplace_back(std::move(window));
	return interface;
}

void SFSEMenuFramework::WindowManager::RenderOpenWindows()
{
	for (const auto& window : windows) {
		if (window->Interface.IsOpen.load(std::memory_order_acquire)) {
			window->Render();
		}
	}
}

bool SFSEMenuFramework::WindowManager::SetMainWindow(Model::WindowInterface* a_window) noexcept
{
	if (!a_window || mainWindow) {
		return false;
	}

	mainWindow = a_window;
	return true;
}

SFSEMenuFramework::Model::WindowInterface* SFSEMenuFramework::WindowManager::GetMainWindow() noexcept
{
	return mainWindow;
}
