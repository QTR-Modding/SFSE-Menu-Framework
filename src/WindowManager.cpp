#include "WindowManager.h"

#include "EventManager.h"
#include "PanelRegistry.h"
#include "Win32Platform.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <vector>

namespace
{
	constexpr std::size_t maximumWindowCount = 1024;

	// The process-lifetime WindowInterface/AddWindow model and aggregate blocking
	// semantics are adapted from SKSE Menu Framework 3 at commit
	// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0). Snapshot publication,
	// render-generation leases, and host-thread notification are Starfield-specific.
	struct Window final
	{
		SFSEMenuFramework::WindowInterface       Interface;
		SFSEMenuFramework::WindowRenderFunction BuiltInRender{ nullptr };
		SFSEMenuFramework::Model::WindowRenderFunction ExternalRender{ nullptr };
		void*                                    UserData{ nullptr };
		bool                                     WasBlockingOpen{ false };
	};

	using WindowSnapshot = std::vector<Window*>;

	struct Registry final
	{
		std::mutex                                      MutationMutex;
		std::vector<std::unique_ptr<Window>>             Windows;
		std::atomic<std::shared_ptr<const WindowSnapshot>> Published;
		std::atomic<SFSEMenuFramework::WindowInterface*> MainWindow{ nullptr };
		std::mutex                                      MainTransitionMutex;
		std::mutex                                      StateMutex;
		bool                                            RenderEnabled{ false };
		bool                                            AnyOpen{ false };
		bool                                            AnyBlocking{ false };
		bool                                            PauseGame{ false };
		bool                                            BlurBackground{ false };
		bool                                            MainOpen{ false };
		std::uint64_t                                   BlockingGeneration{ 0 };
		std::uint64_t                                   MainSessionGeneration{ 0 };
		std::atomic<bool>                               HotkeyEnabled{ true };
	};

	struct AggregateState final
	{
		bool          AnyOpen{ false };
		bool          AnyBlocking{ false };
		bool          PauseGame{ false };
		bool          BlurBackground{ false };
		bool          MainOpen{ false };
		bool          RenderEnabled{ false };
		bool          Changed{ false };
		std::uint32_t BlockingOpenEdges{ 0 };
		std::uint64_t BlockingGeneration{ 0 };
		std::uint64_t MainSessionGeneration{ 0 };
	};

	[[nodiscard]] Registry* GetRegistry() noexcept
	{
		static auto* registry = new (std::nothrow) Registry();
		return registry;
	}

	void IncrementGeneration(std::uint64_t& a_generation) noexcept
	{
		++a_generation;
		if (a_generation == 0) {
			++a_generation;
		}
	}

	[[nodiscard]] AggregateState InspectWindows(Registry& a_registry) noexcept
	{
		AggregateState result;
		const auto snapshot = a_registry.Published.load(std::memory_order_acquire);
		if (snapshot) {
			for (auto* window : *snapshot) {
				if (!window ||
					!window->Interface.IsOpen.load(std::memory_order_acquire)) {
					if (window) {
						window->WasBlockingOpen = false;
					}
					continue;
				}

				result.AnyOpen = true;
				const bool blocking =
					window->Interface.BlockUserInput.load(std::memory_order_acquire);
				if (blocking && !window->WasBlockingOpen) {
					++result.BlockingOpenEdges;
				}
				window->WasBlockingOpen = blocking;
				if (!blocking) {
					continue;
				}

				result.AnyBlocking = true;
			}
		}

		const auto* mainWindow =
			a_registry.MainWindow.load(std::memory_order_acquire);
		result.MainOpen = mainWindow &&
			mainWindow->IsOpen.load(std::memory_order_acquire);
		if (result.AnyBlocking) {
			// SKSE Menu Framework applies its global freeze/blur settings to the
			// aggregate blocking state, including consumer-owned windows.
			result.PauseGame = !mainWindow ||
				mainWindow->PauseGame.load(std::memory_order_acquire);
			result.BlurBackground = !mainWindow ||
				mainWindow->BlurBackground.load(std::memory_order_acquire);
		}
		return result;
	}

	[[nodiscard]] AggregateState RefreshState(Registry& a_registry) noexcept
	{
		std::scoped_lock lock{ a_registry.StateMutex };
		auto current = InspectWindows(a_registry);
		current.Changed = current.AnyOpen != a_registry.AnyOpen ||
			current.AnyBlocking != a_registry.AnyBlocking ||
			current.PauseGame != a_registry.PauseGame ||
			current.BlurBackground != a_registry.BlurBackground ||
			current.MainOpen != a_registry.MainOpen ||
			current.BlockingOpenEdges != 0;

		for (std::uint32_t edge = 0; edge < current.BlockingOpenEdges; ++edge) {
			IncrementGeneration(a_registry.BlockingGeneration);
		}
		if (current.MainOpen && !a_registry.MainOpen) {
			IncrementGeneration(a_registry.MainSessionGeneration);
		}
		a_registry.AnyOpen = current.AnyOpen;
		a_registry.AnyBlocking = current.AnyBlocking;
		a_registry.PauseGame = current.PauseGame;
		a_registry.BlurBackground = current.BlurBackground;
		a_registry.MainOpen = current.MainOpen;
		current.RenderEnabled = a_registry.RenderEnabled;
		current.BlockingGeneration = current.AnyBlocking ?
			a_registry.BlockingGeneration :
			0;
		current.MainSessionGeneration = current.MainOpen ?
			a_registry.MainSessionGeneration :
			0;
		return current;
	}

	void NotifyHostWindow(bool a_changed) noexcept
	{
		if (a_changed) {
			static_cast<void>(
				SFSEMenuFramework::Win32Platform::PostHostWindowCallback());
		}
	}

	[[nodiscard]] AggregateState ReadState(Registry& a_registry) noexcept
	{
		auto state = RefreshState(a_registry);
		NotifyHostWindow(state.Changed);
		return state;
	}

	[[nodiscard]] AggregateState ReadState() noexcept
	{
		auto* registry = GetRegistry();
		return registry ? ReadState(*registry) : AggregateState{};
	}

	template <class Transition>
	[[nodiscard]] bool ApplyMainWindowTransition(
		Transition a_transition,
		bool&      a_result) noexcept
	{
		auto* registry = GetRegistry();
		auto* window = registry ?
			registry->MainWindow.load(std::memory_order_acquire) :
			nullptr;
		if (!registry || !window) {
			return false;
		}

		{
			std::scoped_lock lock{ registry->MainTransitionMutex };
			if (!a_transition(window->IsOpen, a_result)) {
				return false;
			}
			// SKSE-MF's Close restores the main/config blocking defaults after
			// Resume Game. Store only after IsOpen changed so a close cannot expose
			// a transient blocking-open edge to aggregate-state observers.
			window->BlockUserInput.store(true, std::memory_order_release);
			static_cast<void>(RefreshState(*registry));
		}
		static_cast<void>(
			SFSEMenuFramework::Win32Platform::PostHostWindowCallback());
		return true;
	}

	[[nodiscard]] SFSEMenuFramework::WindowInterface* PublishWindow(
		Registry& a_registry,
		std::unique_ptr<Window> a_window)
	{
		std::scoped_lock lock{ a_registry.MutationMutex };
		const auto current =
			a_registry.Published.load(std::memory_order_acquire);
		if (current && current->size() >= maximumWindowCount) {
			return nullptr;
		}

		auto next = current ?
			std::make_shared<WindowSnapshot>(*current) :
			std::make_shared<WindowSnapshot>();
		next->reserve(next->size() + 1);
		auto* rawWindow = a_window.get();
		a_registry.Windows.emplace_back(std::move(a_window));
		next->push_back(rawWindow);
		a_registry.Published.store(std::move(next), std::memory_order_release);
		return &rawWindow->Interface;
	}
}

SFSEMenuFramework::WindowInterface* SFSEMenuFramework::WindowManager::AddWindow(
	WindowRenderFunction a_renderFunction)
{
	if (!a_renderFunction) {
		return nullptr;
	}

	try {
		auto* registry = GetRegistry();
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

SFSEMenuFramework::Model::RegistrationResult
	SFSEMenuFramework::WindowManager::RegisterWindow(
		const Model::WindowRegistration* a_registration,
		Model::WindowInterface**          a_window) noexcept
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
	if (!SFSEMenuFramework::Detail::HasMatchingImGuiLayout(
			a_registration->ImGui)) {
		return Model::RegistrationResult::ImGuiMismatch;
	}
	if (!SFSEMenuFramework::Detail::IsExecutableImageFunction(
			a_registration->Render)) {
		return Model::RegistrationResult::InvalidArgument;
	}

	try {
		auto* registry = GetRegistry();
		if (!registry) {
			return Model::RegistrationResult::OutOfMemory;
		}
		auto window = std::make_unique<Window>();
		window->ExternalRender = a_registration->Render;
		window->UserData = a_registration->UserData;
		window->Interface.BlockUserInput.store(
			a_registration->BlockUserInput != 0,
			std::memory_order_relaxed);
		auto* windowInterface = PublishWindow(*registry, std::move(window));
		if (!windowInterface) {
			return Model::RegistrationResult::RegistryFull;
		}
		*a_window = windowInterface;
		return Model::RegistrationResult::Success;
	} catch (const std::bad_alloc&) {
		return Model::RegistrationResult::OutOfMemory;
	} catch (const std::system_error&) {
		return Model::RegistrationResult::InternalError;
	}
}

std::uint64_t SFSEMenuFramework::WindowManager::RenderOpenWindows(
	const Model::RenderContext& a_context)
{
	auto* registry = GetRegistry();
	if (!registry) {
		return 0;
	}

	const auto before = ReadState(*registry);
	if (!before.RenderEnabled) {
		return 0;
	}

	bool renderedBlockingWindow{};
	const auto snapshot = registry->Published.load(std::memory_order_acquire);
	if (snapshot) {
		for (const auto* window : *snapshot) {
			if (!window ||
				!window->Interface.IsOpen.load(std::memory_order_acquire)) {
				continue;
			}

			const bool blocking =
				window->Interface.BlockUserInput.load(std::memory_order_acquire);
			if (window->BuiltInRender) {
				window->BuiltInRender(a_context);
			} else if (window->ExternalRender) {
				window->ExternalRender(&a_context, window->UserData);
			}
			renderedBlockingWindow = renderedBlockingWindow || blocking;
		}
	}

	const auto after = ReadState(*registry);
	return renderedBlockingWindow &&
		before.BlockingGeneration != 0 &&
		after.BlockingGeneration == before.BlockingGeneration ?
		before.BlockingGeneration :
		0;
}

bool SFSEMenuFramework::WindowManager::SetMainWindow(
	WindowInterface* a_window) noexcept
{
	if (!a_window) {
		return false;
	}
	auto* registry = GetRegistry();
	if (!registry) {
		return false;
	}

	const auto snapshot = registry->Published.load(std::memory_order_acquire);
	bool registered{};
	if (snapshot) {
		for (const auto* window : *snapshot) {
			registered = registered || (window && &window->Interface == a_window);
		}
	}
	if (!registered) {
		return false;
	}

	auto* expected = static_cast<WindowInterface*>(nullptr);
	return registry->MainWindow.compare_exchange_strong(
		expected,
		a_window,
		std::memory_order_release,
		std::memory_order_acquire);
}

SFSEMenuFramework::WindowInterface*
	SFSEMenuFramework::WindowManager::GetMainWindow() noexcept
{
	const auto* registry = GetRegistry();
	return registry ?
		registry->MainWindow.load(std::memory_order_acquire) :
		nullptr;
}

void SFSEMenuFramework::WindowManager::SetMainWindowRenderEnabled(
	bool a_enabled) noexcept
{
	auto* registry = GetRegistry();
	if (!registry) {
		return;
	}
	const auto observed = RefreshState(*registry);
	bool changed{};
	{
		std::scoped_lock lock{ registry->StateMutex };
		if (registry->RenderEnabled != a_enabled) {
			if (registry->RenderEnabled && !a_enabled) {
				if (registry->AnyBlocking) {
					IncrementGeneration(registry->BlockingGeneration);
				}
			}
			registry->RenderEnabled = a_enabled;
			changed = true;
		}
	}
	NotifyHostWindow(observed.Changed || changed);
}

bool SFSEMenuFramework::WindowManager::SetMainWindowOpen(bool a_open) noexcept
{
	bool ignored{};
	return ApplyMainWindowTransition(
		[a_open](std::atomic<bool>& a_state, bool&) {
			return EventManager::SetMainWindowState(a_state, a_open);
		},
		ignored);
}

bool SFSEMenuFramework::WindowManager::IsAnyWindowOpen() noexcept
{
	return ReadState().AnyOpen;
}

bool SFSEMenuFramework::WindowManager::IsAnyBlockingWindowOpened() noexcept
{
	return ReadState().AnyBlocking;
}

bool SFSEMenuFramework::WindowManager::ShouldPauseGame() noexcept
{
	return ReadState().PauseGame;
}

bool SFSEMenuFramework::WindowManager::ShouldBlurBackground() noexcept
{
	return ReadState().BlurBackground;
}

void SFSEMenuFramework::WindowManager::CloseAllBlockingWindows() noexcept
{
	auto* registry = GetRegistry();
	if (!registry) {
		return;
	}

	bool changed{};
	AggregateState state;
	{
		std::scoped_lock lock{ registry->MainTransitionMutex };
		auto* const mainWindow =
			registry->MainWindow.load(std::memory_order_acquire);
		const auto snapshot =
			registry->Published.load(std::memory_order_acquire);
		if (snapshot) {
			for (auto* window : *snapshot) {
				if (!window ||
					!window->Interface.BlockUserInput.load(
						std::memory_order_acquire)) {
					continue;
				}

				if (&window->Interface == mainWindow) {
					changed =
						EventManager::SetMainWindowState(
							window->Interface.IsOpen,
							false,
							true) ||
						changed;
				} else {
					changed =
						window->Interface.IsOpen.exchange(
							false,
							std::memory_order_acq_rel) ||
						changed;
				}
			}
		}
		state = RefreshState(*registry);
	}
	NotifyHostWindow(changed || state.Changed);
}

void SFSEMenuFramework::WindowManager::SetHotkeyEnabled(bool a_enabled) noexcept
{
	if (auto* registry = GetRegistry()) {
		registry->HotkeyEnabled.store(a_enabled, std::memory_order_release);
	}
}

bool SFSEMenuFramework::WindowManager::IsHotkeyEnabled() noexcept
{
	const auto* registry = GetRegistry();
	return registry && registry->HotkeyEnabled.load(std::memory_order_acquire);
}

std::uint64_t
	SFSEMenuFramework::WindowManager::GetBlockingWindowOpenGeneration() noexcept
{
	return ReadState().BlockingGeneration;
}

bool SFSEMenuFramework::WindowManager::IsBlockingWindowOpenGeneration(
	std::uint64_t a_generation) noexcept
{
	const auto state = ReadState();
	return a_generation != 0 && state.AnyBlocking &&
		state.BlockingGeneration == a_generation;
}

std::uint64_t
	SFSEMenuFramework::WindowManager::GetMainWindowSessionGeneration() noexcept
{
	return ReadState().MainSessionGeneration;
}
