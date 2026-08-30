#include "WindowManager.h"

#include <memory>
#include <mutex>
#include <vector>

namespace
{
	struct Window final
	{
		SFSEMenuFramework::WindowInterface       Interface;
		SFSEMenuFramework::WindowRenderFunction Render{ nullptr };
	};

	std::vector<std::unique_ptr<Window>> windows;
	SFSEMenuFramework::WindowInterface*  mainWindow{ nullptr };

	[[nodiscard]] std::mutex& GetMainWindowStateMutex()
	{
		static auto* mutex = new std::mutex();
		return *mutex;
	}
}

SFSEMenuFramework::WindowInterface* SFSEMenuFramework::WindowManager::AddWindow(
	WindowRenderFunction a_renderFunction)
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

std::uint64_t SFSEMenuFramework::WindowManager::RenderOpenWindows(
	const Model::RenderContext& a_context)
{
	std::uint64_t renderedMainWindowGeneration{};
	for (const auto& window : windows) {
		if (&window->Interface == mainWindow) {
			renderedMainWindowGeneration = GetMainWindowOpenGeneration();
			if (renderedMainWindowGeneration != 0) {
				window->Render(a_context);
			}
		} else if (window->Interface.IsOpen.load(std::memory_order_acquire)) {
			window->Render(a_context);
		}
	}
	return renderedMainWindowGeneration;
}

bool SFSEMenuFramework::WindowManager::SetMainWindow(WindowInterface* a_window) noexcept
{
	if (!a_window || mainWindow) {
		return false;
	}

	mainWindow = a_window;
	return true;
}

SFSEMenuFramework::WindowInterface* SFSEMenuFramework::WindowManager::GetMainWindow() noexcept
{
	return mainWindow;
}

bool SFSEMenuFramework::WindowManager::SetMainWindowOpen(bool a_open) noexcept
{
	std::scoped_lock lock{ GetMainWindowStateMutex() };
	if (!mainWindow ||
		mainWindow->IsOpen.load(std::memory_order_relaxed) == a_open) {
		return false;
	}

	if (a_open) {
		mainWindow->OpenGeneration.fetch_add(1, std::memory_order_relaxed);
	}
	mainWindow->IsOpen.store(a_open, std::memory_order_release);
	return true;
}

bool SFSEMenuFramework::WindowManager::ToggleMainWindow() noexcept
{
	std::scoped_lock lock{ GetMainWindowStateMutex() };
	if (!mainWindow) {
		return false;
	}

	const bool isOpen = !mainWindow->IsOpen.load(std::memory_order_relaxed);
	if (isOpen) {
		mainWindow->OpenGeneration.fetch_add(1, std::memory_order_relaxed);
	}
	mainWindow->IsOpen.store(isOpen, std::memory_order_release);
	return isOpen;
}

std::uint64_t SFSEMenuFramework::WindowManager::GetMainWindowOpenGeneration() noexcept
{
	std::scoped_lock lock{ GetMainWindowStateMutex() };
	const auto* window = mainWindow;
	if (!window || !window->IsOpen.load(std::memory_order_relaxed)) {
		return 0;
	}

	return window->OpenGeneration.load(std::memory_order_relaxed);
}

bool SFSEMenuFramework::WindowManager::IsMainWindowOpenGeneration(
	std::uint64_t a_generation) noexcept
{
	if (a_generation == 0) {
		return false;
	}

	std::scoped_lock lock{ GetMainWindowStateMutex() };
	return mainWindow &&
	       mainWindow->IsOpen.load(std::memory_order_relaxed) &&
	       mainWindow->OpenGeneration.load(std::memory_order_relaxed) == a_generation;
}
