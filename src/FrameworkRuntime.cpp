#include "FrameworkRuntime.h"
#include "Win32Platform.h"
#include <imgui.h>
#include <imgui_internal.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <vector>

// Event types, listener priority, windows, and aggregate blocking behavior are
// adapted from SKSE Menu Framework 3 commit 928e01a (GPL-3.0). Immutable
// snapshots, quiescent unregister, deferred transitions, and generation leases
// are Starfield-specific.
namespace SFSEMenuFramework::Detail
{
	struct EventListener final
	{
		static constexpr std::uint32_t ACTIVE = 0x80000000U;
		static constexpr std::uint32_t IN_FLIGHT = ~ACTIVE;
		Model::EventHandle         Handle{ 0 };
		Model::EventCallback       Callback{ nullptr };
		float                      Priority{ 0.0F };
		std::atomic<std::uint32_t> State{ ACTIVE };
		[[nodiscard]] bool IsActive() const noexcept
		{
			return (State.load(std::memory_order_acquire) & ACTIVE) != 0;
		}
		[[nodiscard]] bool Enter() noexcept
		{
			auto state = State.load(std::memory_order_acquire);
			while ((state & ACTIVE) != 0 && (state & IN_FLIGHT) != IN_FLIGHT) {
				if (State.compare_exchange_weak(
						state, state + 1, std::memory_order_acq_rel,
						std::memory_order_acquire)) {
					return true;
				}
			}
			return false;
		}
		void Leave() noexcept
		{
			const auto previous = State.fetch_sub(1, std::memory_order_acq_rel);
			if ((previous & IN_FLIGHT) == 1 || (previous & ACTIVE) == 0) {
				State.notify_all();
			}
		}
	};

	using EventListenerPointer = std::shared_ptr<EventListener>;

	struct EventSnapshot final
	{
		std::vector<EventListenerPointer> Listeners;
	};
}

namespace SFSEMenuFramework
{
	namespace
	{
		using Listener = Detail::EventListener;
		using SnapshotPointer = EventManager::Snapshot;
		constexpr std::size_t maximumListenerCount = 1024;
		constexpr std::size_t maximumPendingTransitions = 1024;
		struct PendingTransition final
		{
			Model::EventType Type{ Model::EventType::kNone };
			SnapshotPointer  Listeners;
		};
		struct EventRegistry final
		{
			std::mutex                  MutationMutex;
			std::atomic<SnapshotPointer> Published;
			Model::EventHandle           NextHandle{ 1 };
			std::mutex                  TransitionMutex;
			std::array<PendingTransition, maximumPendingTransitions + 1> Queue{};
			std::size_t                 Head{};
			std::size_t                 Count{};
			std::atomic_flag            FullLogged{};
			std::atomic_flag            BacklogLogged{};
			[[nodiscard]] bool Push(
				Model::EventType a_type, const SnapshotPointer& a_snapshot,
				bool a_emergencyClose) noexcept
			{
				if (!a_snapshot || !std::ranges::any_of(
						a_snapshot->Listeners, [](const auto& a_listener) {
							return a_listener && a_listener->IsActive();
						})) {
					return true;
				}
				const auto capacity = maximumPendingTransitions +
					(a_emergencyClose ? 1U : 0U);
				if (Count >= capacity) {
					if (!FullLogged.test_and_set(std::memory_order_relaxed)) {
						logger::critical(
							"Lifecycle transition queue reached {} entries", Count);
					}
					return false;
				}
				Queue[(Head + Count) % Queue.size()] = { a_type, a_snapshot };
				++Count;
				return true;
			}
			[[nodiscard]] PendingTransition Pop() noexcept
			{
				auto transition = std::move(Queue[Head]);
				Queue[Head] = {};
				Head = (Head + 1) % Queue.size();
				--Count;
				return transition;
			}
			void CompleteDrain(EventManager::Snapshot& a_snapshot) noexcept
			{
				FullLogged.clear(std::memory_order_relaxed);
				BacklogLogged.clear(std::memory_order_relaxed);
				a_snapshot = Published.load(std::memory_order_acquire);
			}
		};
		thread_local Listener* executingListener{};
		[[nodiscard]] EventRegistry* GetEventRegistry() noexcept
		{
			static auto* registry = new (std::nothrow) EventRegistry();
			return registry;
		}
	}

	Model::RegistrationResult EventManager::Register(
		const Model::EventRegistration* a_registration,
		Model::EventHandle*              a_handle) noexcept
	{
		if (a_handle) {
			*a_handle = 0;
		}
		if (!a_registration || !a_handle) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (a_registration->StructureSize < sizeof(Model::EventRegistration)) {
			return Model::RegistrationResult::StructureTooSmall;
		}
		if (a_registration->InterfaceVersion != Model::INTERFACE_VERSION_3) {
			return Model::RegistrationResult::UnsupportedVersion;
		}
		if (!a_registration->Callback ||
			!std::isfinite(a_registration->Priority) ||
			!Detail::IsExecutableImageFunction(a_registration->Callback)) {
			return Model::RegistrationResult::InvalidArgument;
		}
		try {
			auto listener = std::make_shared<Listener>();
			listener->Callback = a_registration->Callback;
			listener->Priority = a_registration->Priority;
			auto* registry = GetEventRegistry();
			if (!registry) {
				return Model::RegistrationResult::OutOfMemory;
			}
			std::scoped_lock lock{ registry->MutationMutex };
			const auto current = registry->Published.load(std::memory_order_acquire);
			auto next = std::make_shared<Detail::EventSnapshot>();
			if (current) {
				next->Listeners.reserve(current->Listeners.size() + 1);
				for (const auto& registered : current->Listeners) {
					if (registered && registered->IsActive()) {
						next->Listeners.push_back(registered);
					}
				}
			}
			if (next->Listeners.size() >= maximumListenerCount ||
				registry->NextHandle == 0) {
				return Model::RegistrationResult::RegistryFull;
			}
			listener->Handle = registry->NextHandle++;
			const auto registeredHandle = listener->Handle;
			next->Listeners.push_back(std::move(listener));
			std::stable_sort(
				next->Listeners.begin(),
				next->Listeners.end(),
				[](const auto& a, const auto& b) {
					return a->Priority == b->Priority ?
						a->Handle < b->Handle : a->Priority > b->Priority;
				});
			std::scoped_lock transitionLock{ registry->TransitionMutex };
			registry->Published.store(std::move(next), std::memory_order_release);
			*a_handle = registeredHandle;
			return Model::RegistrationResult::Success;
		} catch (const std::bad_alloc&) {
			return Model::RegistrationResult::OutOfMemory;
		} catch (const std::system_error&) {
			return Model::RegistrationResult::InternalError;
		}
	}

	void EventManager::Unregister(Model::EventHandle a_handle) noexcept
	{
		if (a_handle == 0) {
			return;
		}
		const auto* registry = GetEventRegistry();
		const auto snapshot = registry ?
			registry->Published.load(std::memory_order_acquire) : nullptr;
		if (!snapshot) {
			return;
		}
		for (const auto& listener : snapshot->Listeners) {
			if (!listener || listener->Handle != a_handle) {
				continue;
			}
			listener->State.fetch_and(Listener::IN_FLIGHT, std::memory_order_acq_rel);
			const std::uint32_t allowedInFlight =
				executingListener == listener.get() ? 1U : 0U;
			auto state = listener->State.load(std::memory_order_acquire);
			while ((state & Listener::IN_FLIGHT) > allowedInFlight) {
				listener->State.wait(state, std::memory_order_acquire);
				state = listener->State.load(std::memory_order_acquire);
			}
			return;
		}
	}

	bool EventManager::SetMainWindowState(
		std::atomic<bool>& a_state,
		bool               a_open,
		bool               a_emergencyClose) noexcept
	{
		auto* registry = GetEventRegistry();
		if (!registry) {
			const bool previous =
				a_state.exchange(a_open, std::memory_order_acq_rel);
			return previous != a_open;
		}
		std::scoped_lock lock{ registry->TransitionMutex };
		if (a_state.load(std::memory_order_acquire) == a_open) {
			return false;
		}
		const auto snapshot = registry->Published.load(std::memory_order_acquire);
		if (!registry->Push(
				a_open ? Model::EventType::kOpenMenu :
					Model::EventType::kCloseMenu,
				snapshot,
				a_emergencyClose && !a_open)) {
			if (a_emergencyClose && !a_open) {
				logger::critical(
					"Applying an emergency main-window close without lifecycle delivery");
				a_state.store(false, std::memory_order_release);
				return true;
			}
			return false;
		}
		a_state.store(a_open, std::memory_order_release);
		return true;
	}

	bool EventManager::BeginFrame(Snapshot& a_snapshot) noexcept
	{
		a_snapshot.reset();
		auto* registry = GetEventRegistry();
		if (!registry) {
			return true;
		}
		for (std::size_t dispatched = 0;
			dispatched < registry->Queue.size(); ++dispatched) {
			PendingTransition transition;
			{
				std::scoped_lock lock{ registry->TransitionMutex };
				if (registry->Count == 0) {
					registry->CompleteDrain(a_snapshot);
					return true;
				}
				transition = registry->Pop();
			}
			Dispatch(transition.Type, transition.Listeners);
		}
		std::scoped_lock lock{ registry->TransitionMutex };
		if (registry->Count == 0) {
			registry->CompleteDrain(a_snapshot);
			return true;
		}
		if (!registry->BacklogLogged.test_and_set(
				std::memory_order_relaxed)) {
			logger::critical(
				"Lifecycle transitions are oscillating; deferring a frame with {} events pending",
				registry->Count);
		}
		return false;
	}

	void EventManager::Dispatch(
		Model::EventType a_type,
		const Snapshot&  a_snapshot) noexcept
	{
		if (a_type < Model::EventType::kOpenMenu ||
			a_type > Model::EventType::kAfterRender || !a_snapshot) {
			return;
		}
		for (const auto& listener : a_snapshot->Listeners) {
			if (!listener || !listener->Callback || !listener->Enter()) {
				continue;
			}
			auto* const previousListener = executingListener;
			executingListener = listener.get();
			listener->Callback(a_type);
			executingListener = previousListener;
			listener->Leave();
		}
	}
}

// ---- Panel registry ------------------------------------------------------------

namespace SFSEMenuFramework
{
	namespace Detail
	{
		bool HasMatchingImGuiLayout(
			const Model::ImGuiLayout& a_layout) noexcept
		{
			return a_layout.StructureSize >= sizeof(Model::ImGuiLayout) &&
			       a_layout.VersionNumber == IMGUI_VERSION_NUM &&
			       a_layout.SourceRevision == Model::IMGUI_SOURCE_REVISION &&
			       a_layout.ConfigurationFlags == 0 &&
			       a_layout.IoSize == sizeof(ImGuiIO) &&
			       a_layout.StyleSize == sizeof(ImGuiStyle) &&
			       a_layout.ContextSize == sizeof(ImGuiContext) &&
			       a_layout.Vec2Size == sizeof(ImVec2) &&
			       a_layout.Vec4Size == sizeof(ImVec4) &&
			       a_layout.DrawVertSize == sizeof(ImDrawVert) &&
			       a_layout.DrawIdxSize == sizeof(ImDrawIdx) &&
			       a_layout.DrawCmdSize == sizeof(ImDrawCmd) &&
			       a_layout.TextureIdSize == sizeof(ImTextureID) &&
			       a_layout.WcharSize == sizeof(ImWchar);
		}
		bool IsExecutableImageAddress(
			const void* a_address,
			void**      a_ownerModule) noexcept
		{
			MEMORY_BASIC_INFORMATION information{};
			if (::VirtualQuery(a_address, &information, sizeof(information)) !=
					sizeof(information) ||
				information.State != MEM_COMMIT ||
				information.Type != MEM_IMAGE ||
				(information.Protect & PAGE_GUARD) != 0 ||
				!information.AllocationBase) {
				return false;
			}
			const auto protection = information.Protect & 0xFF;
			const bool executable = protection == PAGE_EXECUTE ||
				protection == PAGE_EXECUTE_READ ||
				protection == PAGE_EXECUTE_READWRITE ||
				protection == PAGE_EXECUTE_WRITECOPY;
			if (executable && a_ownerModule) {
				*a_ownerModule = information.AllocationBase;
			}
			return executable;
		}
	}

	namespace
	{
		constexpr std::size_t maximumPanelCount = 1024;
		struct PanelRegistryState final
		{
			std::mutex                                 Mutex;
			std::vector<PanelRegistry::PanelPointer>   Panels;
			std::atomic<PanelRegistry::MenuTreePointer> Roots;
			Model::PanelHandle                          NextHandle{ 1 };
		};
		[[nodiscard]] PanelRegistryState* GetPanelRegistryState() noexcept
		{
			static auto* registry = new (std::nothrow) PanelRegistryState();
			return registry;
		}
		[[nodiscard]] bool IsValidText(
			const Model::StringView& a_text,
			std::uint32_t            a_maximumLength) noexcept
		{
			return a_text.Data && a_text.Size && a_text.Size <= a_maximumLength &&
				!std::memchr(a_text.Data, '\0', a_text.Size);
		}
		void AddToMenuTree(
			PanelRegistryState&                          a_registry,
			const PanelRegistry::PanelPointer& a_panel)
		{
			const std::string path = a_panel->Section + '/' + a_panel->Title;
			const std::string_view pathView{ path };
			std::vector<std::string_view> parts;
			for (std::size_t begin = 0; begin < path.size();) {
				const auto slash = path.find('/', begin);
				parts.push_back(pathView.substr(
					begin,
					slash == std::string::npos ? slash : slash - begin));
				if (slash == std::string::npos) {
					break;
				}
				begin = slash + 1;
			}
			auto* children = &a_registry.Roots;
			PanelRegistry::MenuNodePointer node;
			std::size_t firstMissing{};
			for (; firstMissing < parts.size(); ++firstMissing) {
				const auto nodes = children->load(std::memory_order_acquire);
				const auto found = nodes ? std::ranges::find_if(
					*nodes, [&](const auto& entry) {
						return entry && entry->Name == parts[firstMissing];
					}) : PanelRegistry::MenuTree::const_iterator{};
				node = nodes && found != nodes->end() ? *found : nullptr;
				if (!node) {
					break;
				}
				children = &node->Children;
			}
			if (firstMissing == parts.size()) {
				node->Panel.store(a_panel, std::memory_order_release);
				return;
			}
			// Allocate the complete missing branch before publishing its root.
			// A failed allocation therefore cannot expose partial empty nodes.
			PanelRegistry::MenuNodePointer branch;
			for (auto index = parts.size(); index-- > firstMissing;) {
				node = std::make_shared<PanelRegistry::MenuNode>();
				node->Name = parts[index];
				const auto prefixLength = static_cast<std::size_t>(
					parts[index].data() - path.data()) + parts[index].size();
				node->FullPath.assign(path.data(), prefixLength);
				if (branch) {
					auto list = std::make_shared<PanelRegistry::MenuTree>();
					list->push_back(std::move(branch));
					node->Children.store(std::move(list), std::memory_order_relaxed);
				} else {
					node->Panel.store(a_panel, std::memory_order_relaxed);
				}
				branch = std::move(node);
			}
			const auto current = children->load(std::memory_order_acquire);
			auto next = current ?
			                std::make_shared<PanelRegistry::MenuTree>(*current) :
			                std::make_shared<PanelRegistry::MenuTree>();
			next->push_back(std::move(branch));
			children->store(std::move(next), std::memory_order_release);
		}
	}

	Model::RegistrationResult PanelRegistry::Register(
		const Model::PanelRegistration* a_registration,
		Model::PanelHandle*              a_handle) noexcept
	{
		if (a_handle) {
			*a_handle = 0;
		}
		if (!a_registration) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (a_registration->StructureSize < sizeof(Model::PanelRegistration) ||
			a_registration->ImGui.StructureSize < sizeof(Model::ImGuiLayout)) {
			return Model::RegistrationResult::StructureTooSmall;
		}
		if (a_registration->InterfaceVersion != Model::INTERFACE_VERSION) {
			return Model::RegistrationResult::UnsupportedVersion;
		}
		if (!a_registration->Render ||
			!IsValidText(a_registration->Id, Model::MAXIMUM_PANEL_ID_LENGTH) ||
			!IsValidText(a_registration->Section, Model::MAXIMUM_PANEL_TEXT_LENGTH) ||
			!IsValidText(a_registration->Title, Model::MAXIMUM_PANEL_TEXT_LENGTH)) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (!Detail::HasMatchingImGuiLayout(a_registration->ImGui)) {
			return Model::RegistrationResult::ImGuiMismatch;
		}
		void* ownerModule{};
		if (!Detail::IsExecutableImageFunction(a_registration->Render, &ownerModule)) {
			return Model::RegistrationResult::InvalidArgument;
		}
		try {
			auto panel = std::make_shared<Panel>();
			panel->OwnerModule = ownerModule;
			panel->Id.assign(a_registration->Id.Data, a_registration->Id.Size);
			panel->Section.assign(
				a_registration->Section.Data, a_registration->Section.Size);
			panel->Title.assign(
				a_registration->Title.Data, a_registration->Title.Size);
			panel->Render = a_registration->Render;
			panel->UserData = a_registration->UserData;
			auto* registry = GetPanelRegistryState();
			if (!registry) {
				return Model::RegistrationResult::OutOfMemory;
			}
			std::scoped_lock lock{ registry->Mutex };
			if (registry->Panels.size() >= maximumPanelCount) {
				return Model::RegistrationResult::RegistryFull;
			}
			registry->Panels.reserve(maximumPanelCount);
			if (std::ranges::any_of(registry->Panels, [&](const auto& registered) {
					return registered->OwnerModule == panel->OwnerModule &&
						registered->Id == panel->Id;
				})) {
				return Model::RegistrationResult::DuplicateId;
			}
			const auto registeredHandle = registry->NextHandle++;
			AddToMenuTree(*registry, panel);
			registry->Panels.push_back(std::move(panel));
			if (a_handle) {
				*a_handle = registeredHandle;
			}
			return Model::RegistrationResult::Success;
		} catch (const std::bad_alloc&) {
			return Model::RegistrationResult::OutOfMemory;
		} catch (const std::system_error&) {
			return Model::RegistrationResult::InternalError;
		}
	}

	PanelRegistry::MenuTreePointer PanelRegistry::GetMenuTree() noexcept
	{
		const auto* registry = GetPanelRegistryState();
		return registry ? registry->Roots.load(std::memory_order_acquire) : nullptr;
	}

	void PanelRegistry::Render(
		const PanelPointer&         a_panel,
		const Model::RenderContext& a_context)
	{
		if (!a_panel ||
			!a_panel->Enabled.load(std::memory_order_acquire) ||
			!a_panel->Render) {
			return;
		}
		const auto result = a_panel->Render(&a_context, a_panel->UserData);
		if (result == Model::PanelRenderResult::Continue) {
			return;
		}
		a_panel->Enabled.store(false, std::memory_order_release);
		if (result == Model::PanelRenderResult::Failed) {
			logger::error("Disabled panel '{} / {}' after its render callback failed",
				a_panel->Section, a_panel->Title);
		} else {
			logger::info("Panel '{} / {}' disabled itself",
				a_panel->Section, a_panel->Title);
		}
	}
}

// ---- Window manager ------------------------------------------------------------

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
// ---- Exported plugin API -------------------------------------------------------

namespace
{
	using namespace SFSEMenuFramework;

	[[nodiscard]] Model::WindowInterface* __stdcall GetMainWindowAPI() noexcept
	{
		return WindowManager::GetMainWindow();
	}

	const Model::Interface interfaceV1{
		.StructureSize = sizeof(Model::Interface),
		.Version = Model::INTERFACE_VERSION,
		.RegisterPanel = &PanelRegistry::Register
	};
	const Model::InterfaceV2 interfaceV2{
		.StructureSize = sizeof(Model::InterfaceV2),
		.Version = Model::INTERFACE_VERSION_2,
		.RegisterPanel = &PanelRegistry::Register,
		.RegisterWindow = &WindowManager::RegisterWindow,
		.GetMainWindow = &GetMainWindowAPI,
		.IsAnyBlockingWindowOpened = &WindowManager::IsAnyBlockingWindowOpened,
		.SetHotkeyEnabled = &WindowManager::SetHotkeyEnabled,
		.IsHotkeyEnabled = &WindowManager::IsHotkeyEnabled
	};
	const Model::InterfaceV3 interfaceV3{
		.StructureSize = sizeof(Model::InterfaceV3),
		.Version = Model::INTERFACE_VERSION_3,
		.RegisterPanel = &PanelRegistry::Register,
		.RegisterWindow = &WindowManager::RegisterWindow,
		.GetMainWindow = &GetMainWindowAPI,
		.IsAnyBlockingWindowOpened = &WindowManager::IsAnyBlockingWindowOpened,
		.SetHotkeyEnabled = &WindowManager::SetHotkeyEnabled,
		.IsHotkeyEnabled = &WindowManager::IsHotkeyEnabled,
		.RegisterEvent = &EventManager::Register,
		.UnregisterEvent = &EventManager::Unregister
	};
}

extern "C" __declspec(dllexport)
	const SFSEMenuFramework::Model::Interface* __stdcall
	SFSEMenuFramework_QueryInterface(std::uint32_t a_version) noexcept
{
	using namespace SFSEMenuFramework::Model;
	switch (a_version) {
	case INTERFACE_VERSION:
		return &interfaceV1;
	case INTERFACE_VERSION_2:
		return reinterpret_cast<const Interface*>(&interfaceV2);
	case INTERFACE_VERSION_3:
		return reinterpret_cast<const Interface*>(&interfaceV3);
	default:
		return nullptr;
	}
}
