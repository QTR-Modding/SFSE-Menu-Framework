#include "runtime/WindowManager.h"

#include "platform/win32/Win32Platform.h"
#include "runtime/ConsumerValidation.h"
#include "runtime/EventManager.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

// Window registration and aggregate blocking behavior are adapted from
// SKSE Menu Framework 3 commit 928e01a (GPL-3.0). Immutable snapshots and
// generation leases are Starfield-specific.
namespace
{
	using namespace SFSEMenuFramework;
	constexpr std::size_t maximumWindowCount = 1024;

	struct Window final
	{
		WindowInterface            Interface;
		WindowRenderFunction       BuiltInRender{ nullptr };
		Model::WindowRenderFunction ExternalRender{ nullptr };
		void*                      UserData{ nullptr };
		bool                       WasBlockingOpen{ false };
	};

	using WindowSnapshot = std::vector<Window*>;

	struct AggregateState final
	{
		bool AnyOpen{};
		bool AnyBlocking{};
		bool PauseGame{};
		bool BlurBackground{};
		bool MainOpen{};
		bool RenderEnabled{};
		bool Changed{};
		std::uint32_t BlockingOpenEdges{};
		std::uint64_t BlockingGeneration{};
		std::uint64_t MainSessionGeneration{};
	};

	struct WindowRegistry final
	{
		std::mutex                                      MutationMutex;
		std::vector<std::unique_ptr<Window>>             Windows;
		std::atomic<std::shared_ptr<const WindowSnapshot>> Published;
		std::atomic<WindowInterface*>                    MainWindow{ nullptr };
		std::mutex                                      MainTransitionMutex;
		std::mutex                                      StateMutex;
		AggregateState                                  Cached;
		bool                                            RenderEnabled{};
		std::atomic<bool>                               HotkeyEnabled{ true };
	};

	[[nodiscard]] WindowRegistry* GetWindowRegistry() noexcept
	{
		static auto* registry = new (std::nothrow) WindowRegistry();
		return registry;
	}

	void Increment(std::uint64_t& a_generation) noexcept
	{
		if (++a_generation == 0) {
			++a_generation;
		}
	}

	[[nodiscard]] AggregateState InspectWindows(WindowRegistry& a_registry) noexcept
	{
		AggregateState state;
		if (const auto windows = a_registry.Published.load(std::memory_order_acquire)) {
			for (auto* window : *windows) {
				if (!window) {
					continue;
				}
				const bool open = window->Interface.IsOpen.load(std::memory_order_acquire);
				const bool blocking = open && window->Interface.BlockUserInput.load(
					std::memory_order_acquire);
				state.AnyOpen |= open;
				state.AnyBlocking |= blocking;
				state.BlockingOpenEdges += blocking && !window->WasBlockingOpen;
				window->WasBlockingOpen = blocking;
			}
		}
		const auto* main = a_registry.MainWindow.load(std::memory_order_acquire);
		state.MainOpen = main && main->IsOpen.load(std::memory_order_acquire);
		if (state.AnyBlocking) {
			state.PauseGame = !main || main->PauseGame.load(std::memory_order_acquire);
			state.BlurBackground = !main ||
				main->BlurBackground.load(std::memory_order_acquire);
		}
		return state;
	}

	[[nodiscard]] AggregateState RefreshState(WindowRegistry& a_registry) noexcept
	{
		std::scoped_lock lock{ a_registry.StateMutex };
		auto state = InspectWindows(a_registry);
		auto& cached = a_registry.Cached;
		state.Changed = state.BlockingOpenEdges || state.AnyOpen != cached.AnyOpen ||
			state.AnyBlocking != cached.AnyBlocking ||
			state.PauseGame != cached.PauseGame ||
			state.BlurBackground != cached.BlurBackground ||
			state.MainOpen != cached.MainOpen;
		for (std::uint32_t edge = 0; edge < state.BlockingOpenEdges; ++edge) {
			Increment(cached.BlockingGeneration);
		}
		if (state.MainOpen && !cached.MainOpen) {
			Increment(cached.MainSessionGeneration);
		}
		cached.AnyOpen = state.AnyOpen;
		cached.AnyBlocking = state.AnyBlocking;
		cached.PauseGame = state.PauseGame;
		cached.BlurBackground = state.BlurBackground;
		cached.MainOpen = state.MainOpen;
		state.RenderEnabled = a_registry.RenderEnabled;
		state.BlockingGeneration = state.AnyBlocking ? cached.BlockingGeneration : 0;
		state.MainSessionGeneration = state.MainOpen ?
			cached.MainSessionGeneration : 0;
		return state;
	}

	void NotifyHostWindow(bool a_changed) noexcept
	{
		if (a_changed) {
			static_cast<void>(Win32Platform::PostHostWindowCallback());
		}
	}

	[[nodiscard]] AggregateState ReadState(WindowRegistry& a_registry) noexcept
	{
		auto state = RefreshState(a_registry);
		NotifyHostWindow(state.Changed);
		return state;
	}

	[[nodiscard]] AggregateState ReadState() noexcept
	{
		auto* registry = GetWindowRegistry();
		return registry ? ReadState(*registry) : AggregateState{};
	}

	[[nodiscard]] WindowInterface* PublishWindow(
		WindowRegistry& a_registry, std::unique_ptr<Window> a_window)
	{
		std::scoped_lock lock{ a_registry.MutationMutex };
		const auto current = a_registry.Published.load(std::memory_order_acquire);
		if (current && current->size() >= maximumWindowCount) {
			return nullptr;
		}
		auto next = current ? std::make_shared<WindowSnapshot>(*current) :
			std::make_shared<WindowSnapshot>();
		next->reserve(next->size() + 1);
		auto* raw = a_window.get();
		a_registry.Windows.push_back(std::move(a_window));
		next->push_back(raw);
		a_registry.Published.store(std::move(next), std::memory_order_release);
		return &raw->Interface;
	}
}
namespace SFSEMenuFramework
{
	WindowInterface* WindowManager::AddWindow(WindowRenderFunction a_renderFunction)
	{
		if (!a_renderFunction) {
			return nullptr;
		}
		try {
			auto* registry = GetWindowRegistry();
			if (!registry) {
				return nullptr;
			}
			auto window = std::make_unique<Window>();
			window->BuiltInRender = a_renderFunction;
			return PublishWindow(*registry, std::move(window));
		} catch (const std::bad_alloc&) {
			return nullptr;
		} catch (const std::system_error&) {
			return nullptr;
		}
	}

	Model::RegistrationResult WindowManager::RegisterWindow(
		const Model::WindowRegistration* a_registration,
		Model::WindowInterface** a_window) noexcept
	{
		if (a_window) {
			*a_window = nullptr;
		}
		if (!a_registration || !a_window) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (a_registration->StructureSize < sizeof(Model::WindowRegistration) ||
			a_registration->ImGui.StructureSize < sizeof(Model::ImGuiLayout)) {
			return Model::RegistrationResult::StructureTooSmall;
		}
		if (a_registration->InterfaceVersion != Model::INTERFACE_VERSION_2) {
			return Model::RegistrationResult::UnsupportedVersion;
		}
		if (!a_registration->Render || a_registration->BlockUserInput > 1) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (!Detail::HasMatchingImGuiLayout(a_registration->ImGui)) {
			return Model::RegistrationResult::ImGuiMismatch;
		}
		if (!Detail::IsExecutableImageFunction(a_registration->Render)) {
			return Model::RegistrationResult::InvalidArgument;
		}
		try {
			auto* registry = GetWindowRegistry();
			if (!registry) {
				return Model::RegistrationResult::OutOfMemory;
			}
			auto window = std::make_unique<Window>();
			window->ExternalRender = a_registration->Render;
			window->UserData = a_registration->UserData;
			window->Interface.BlockUserInput.store(
				a_registration->BlockUserInput != 0, std::memory_order_relaxed);
			*a_window = PublishWindow(*registry, std::move(window));
			return *a_window ? Model::RegistrationResult::Success :
				Model::RegistrationResult::RegistryFull;
		} catch (const std::bad_alloc&) {
			return Model::RegistrationResult::OutOfMemory;
		} catch (const std::system_error&) {
			return Model::RegistrationResult::InternalError;
		}
	}

	std::uint64_t WindowManager::RenderOpenWindows(
		const Model::RenderContext& a_context)
	{
		auto* registry = GetWindowRegistry();
		if (!registry) {
			return 0;
		}
		const auto before = ReadState(*registry);
		if (!before.RenderEnabled) {
			return 0;
		}
		bool renderedBlockingWindow{};
		if (const auto windows = registry->Published.load(std::memory_order_acquire)) {
			for (const auto* window : *windows) {
				if (!window ||
					!window->Interface.IsOpen.load(std::memory_order_acquire)) {
					continue;
				}
				const bool blocking = window->Interface.BlockUserInput.load(
					std::memory_order_acquire);
				if (window->BuiltInRender) {
					window->BuiltInRender(a_context);
				} else if (window->ExternalRender) {
					window->ExternalRender(&a_context, window->UserData);
				}
				renderedBlockingWindow |= blocking;
			}
		}
		const auto after = ReadState(*registry);
		return renderedBlockingWindow && before.BlockingGeneration &&
			after.BlockingGeneration == before.BlockingGeneration ?
			before.BlockingGeneration : 0;
	}

	bool WindowManager::SetMainWindow(WindowInterface* a_window) noexcept
	{
		auto* registry = GetWindowRegistry();
		if (!registry || !a_window) {
			return false;
		}
		const auto windows = registry->Published.load(std::memory_order_acquire);
		if (!windows || !std::ranges::any_of(*windows, [a_window](const auto* entry) {
				return entry && &entry->Interface == a_window;
			})) {
			return false;
		}
		auto* expected = static_cast<WindowInterface*>(nullptr);
		return registry->MainWindow.compare_exchange_strong(
			expected, a_window, std::memory_order_release, std::memory_order_acquire);
	}

	WindowInterface* WindowManager::GetMainWindow() noexcept
	{
		const auto* registry = GetWindowRegistry();
		return registry ? registry->MainWindow.load(std::memory_order_acquire) : nullptr;
	}

	void WindowManager::SetMainWindowRenderEnabled(bool a_enabled) noexcept
	{
		auto* registry = GetWindowRegistry();
		if (!registry) {
			return;
		}
		auto observed = RefreshState(*registry);
		{
			std::scoped_lock lock{ registry->StateMutex };
			if (registry->RenderEnabled != a_enabled) {
				if (registry->RenderEnabled && !a_enabled &&
					registry->Cached.AnyBlocking) {
					Increment(registry->Cached.BlockingGeneration);
				}
				registry->RenderEnabled = a_enabled;
				observed.Changed = true;
			}
		}
		NotifyHostWindow(observed.Changed);
	}

	bool WindowManager::SetMainWindowOpen(bool a_open) noexcept
	{
		auto* registry = GetWindowRegistry();
		auto* window = registry ?
			registry->MainWindow.load(std::memory_order_acquire) : nullptr;
		if (!registry || !window) {
			return false;
		}
		std::scoped_lock lock{ registry->MainTransitionMutex };
		if (!EventManager::SetMainWindowState(window->IsOpen, a_open)) {
			return false;
		}
		window->BlockUserInput.store(true, std::memory_order_release);
		static_cast<void>(RefreshState(*registry));
		static_cast<void>(Win32Platform::PostHostWindowCallback());
		return true;
	}

	bool WindowManager::IsAnyWindowOpen() noexcept { return ReadState().AnyOpen; }
	bool WindowManager::IsAnyBlockingWindowOpened() noexcept
	{
		return ReadState().AnyBlocking;
	}
	bool WindowManager::ShouldPauseGame() noexcept { return ReadState().PauseGame; }
	bool WindowManager::ShouldBlurBackground() noexcept
	{
		return ReadState().BlurBackground;
	}

	void WindowManager::CloseAllBlockingWindows() noexcept
	{
		auto* registry = GetWindowRegistry();
		if (!registry) {
			return;
		}
		bool changed{};
		AggregateState state;
		{
			std::scoped_lock lock{ registry->MainTransitionMutex };
			auto* main = registry->MainWindow.load(std::memory_order_acquire);
			if (const auto windows = registry->Published.load(std::memory_order_acquire)) {
				for (auto* window : *windows) {
					if (!window || !window->Interface.BlockUserInput.load(
							std::memory_order_acquire)) {
						continue;
					}
					const bool closed = &window->Interface == main ?
						EventManager::SetMainWindowState(
							window->Interface.IsOpen, false, true) :
						window->Interface.IsOpen.exchange(
							false, std::memory_order_acq_rel);
					changed |= closed;
				}
			}
			state = RefreshState(*registry);
		}
		NotifyHostWindow(changed || state.Changed);
	}

	void WindowManager::SetHotkeyEnabled(bool a_enabled) noexcept
	{
		if (auto* registry = GetWindowRegistry()) {
			registry->HotkeyEnabled.store(a_enabled, std::memory_order_release);
		}
	}

	bool WindowManager::IsHotkeyEnabled() noexcept
	{
		const auto* registry = GetWindowRegistry();
		return registry && registry->HotkeyEnabled.load(std::memory_order_acquire);
	}

	std::uint64_t WindowManager::GetBlockingWindowOpenGeneration() noexcept
	{
		return ReadState().BlockingGeneration;
	}

	bool WindowManager::IsBlockingWindowOpenGeneration(
		std::uint64_t a_generation) noexcept
	{
		const auto state = ReadState();
		return a_generation && state.AnyBlocking &&
			state.BlockingGeneration == a_generation;
	}

	std::uint64_t WindowManager::GetMainWindowSessionGeneration() noexcept
	{
		return ReadState().MainSessionGeneration;
	}
}
