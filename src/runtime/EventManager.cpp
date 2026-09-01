#include "runtime/EventManager.h"
#include "runtime/ConsumerValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

// Event types and listener priority are adapted from SKSE Menu Framework 3
// commit 928e01a (GPL-3.0). Immutable snapshots, quiescent unregister,
// deferred transitions, and generation leases are Starfield-specific.
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
		if (a_registration->InterfaceVersion != Model::INTERFACE_VERSION) {
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
