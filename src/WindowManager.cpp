#include "WindowManager.h"

#include "Win32Platform.h"

#include <atomic>
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
	std::mutex                           windowsMutex;
	std::atomic<SFSEMenuFramework::WindowInterface*> mainWindow{ nullptr };

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

	auto* windowInterface = &window->Interface;
	{
		std::scoped_lock lock{ windowsMutex };
		windows.emplace_back(std::move(window));
	}
	return windowInterface;
}

std::uint64_t SFSEMenuFramework::WindowManager::RenderOpenWindows(
	const Model::RenderContext& a_context)
{
	std::uint64_t renderedMainWindowGeneration{};
	std::scoped_lock lock{ windowsMutex };
	const auto* currentMainWindow = mainWindow.load(std::memory_order_acquire);
	for (const auto& window : windows) {
		if (&window->Interface == currentMainWindow) {
			const auto openGeneration = GetMainWindowOpenGeneration();
			if (openGeneration != 0 &&
				window->Interface.RenderEnabled.load(std::memory_order_acquire)) {
				window->Render(a_context);
				renderedMainWindowGeneration = openGeneration;
			}
		} else if (window->Interface.IsOpen.load(std::memory_order_acquire)) {
			window->Render(a_context);
		}
	}
	return renderedMainWindowGeneration;
}

bool SFSEMenuFramework::WindowManager::SetMainWindow(WindowInterface* a_window) noexcept
{
	if (!a_window) {
		return false;
	}

	auto* expected = static_cast<WindowInterface*>(nullptr);
	return mainWindow.compare_exchange_strong(
		expected,
		a_window,
		std::memory_order_release,
		std::memory_order_acquire);
}

SFSEMenuFramework::WindowInterface* SFSEMenuFramework::WindowManager::GetMainWindow() noexcept
{
	return mainWindow.load(std::memory_order_acquire);
}

void SFSEMenuFramework::WindowManager::SetMainWindowRenderEnabled(
	bool a_enabled) noexcept
{
	std::scoped_lock lock{ GetMainWindowStateMutex() };
	auto* window = mainWindow.load(std::memory_order_acquire);
	if (!window) {
		return;
	}

	const bool wasEnabled = window->RenderEnabled.exchange(
		a_enabled,
		std::memory_order_acq_rel);
	if (wasEnabled && !a_enabled &&
		window->IsOpen.load(std::memory_order_relaxed)) {
		// A frame from before focus loss or LoadingMenu must never re-arm
		// ownership. A new generation requires one fresh eligible frame.
		window->OpenGeneration.fetch_add(1, std::memory_order_relaxed);
	}
}

bool SFSEMenuFramework::WindowManager::SetMainWindowOpen(bool a_open) noexcept
{
	{
		std::scoped_lock lock{ GetMainWindowStateMutex() };
		auto* window = mainWindow.load(std::memory_order_acquire);
		if (!window || window->IsOpen.load(std::memory_order_relaxed) == a_open) {
			return false;
		}

		if (a_open) {
			window->OpenGeneration.fetch_add(1, std::memory_order_relaxed);
		}
		window->IsOpen.store(a_open, std::memory_order_release);
	}

	static_cast<void>(Win32Platform::PostHostWindowCallback());
	return true;
}

bool SFSEMenuFramework::WindowManager::ToggleMainWindow() noexcept
{
	bool isOpen{};
	{
		std::scoped_lock lock{ GetMainWindowStateMutex() };
		auto* window = mainWindow.load(std::memory_order_acquire);
		if (!window) {
			return false;
		}

		isOpen = !window->IsOpen.load(std::memory_order_relaxed);
		if (isOpen) {
			window->OpenGeneration.fetch_add(1, std::memory_order_relaxed);
		}
		window->IsOpen.store(isOpen, std::memory_order_release);
	}

	static_cast<void>(Win32Platform::PostHostWindowCallback());
	return isOpen;
}

std::uint64_t SFSEMenuFramework::WindowManager::GetMainWindowOpenGeneration() noexcept
{
	std::scoped_lock lock{ GetMainWindowStateMutex() };
	const auto* window = mainWindow.load(std::memory_order_acquire);
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
	const auto* window = mainWindow.load(std::memory_order_acquire);
	return window &&
	       window->IsOpen.load(std::memory_order_relaxed) &&
	       window->OpenGeneration.load(std::memory_order_relaxed) == a_generation;
}
