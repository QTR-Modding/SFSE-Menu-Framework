#include "runtime/EventManager.h"
#include "runtime/ConsumerValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <new>
#include <utility>

// Event types and listener priority are adapted from SKSE Menu Framework 3
// commit 928e01a (GPL-3.0). Immutable snapshots, quiescent unregister,
// deferred transitions, and generation leases are Starfield-specific.
namespace SFSEMenuFramework
{
	namespace
	{
		using SnapshotPointer = EventManager::Snapshot;
		constexpr std::size_t maximumPendingTransitions = 1024;
		struct PendingTransition final
		{
			Model::EventType Type{ Model::EventType::kNone };
			SnapshotPointer  Listeners;
		};
		struct EventRegistry final
		{
			Detail::EventCallbacks      Callbacks;
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
						*a_snapshot, [](const auto& a_listener) {
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
				a_snapshot = Callbacks.CaptureSnapshot();
			}
		};
		[[nodiscard]] EventRegistry* GetEventRegistry() noexcept
		{
			static auto* registry = new (std::nothrow) EventRegistry();
			return registry;
		}
	}

	Model::EventHandle EventManager::Register(
		Model::EventCallback a_callback,
		float                a_priority) noexcept
	{
		if (!a_callback || !std::isfinite(a_priority) ||
			!Detail::IsExecutableImageFunction(a_callback)) {
			return 0;
		}
		auto* registry = GetEventRegistry();
		return registry ? registry->Callbacks.Register(
			a_callback, a_priority, &registry->TransitionMutex) : 0;
	}

	void EventManager::Unregister(Model::EventHandle a_handle) noexcept
	{
		if (a_handle == 0) {
			return;
		}
		if (auto* registry = GetEventRegistry()) {
			registry->Callbacks.Unregister(a_handle);
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
		const auto snapshot = registry->Callbacks.CaptureSnapshot();
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
		Detail::EventCallbacks::Dispatch(
			a_snapshot, [a_type](const auto& a_entry) noexcept {
				a_entry.Function(a_type);
			});
	}
}
