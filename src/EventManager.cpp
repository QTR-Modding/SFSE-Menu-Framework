#include "EventManager.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <vector>

namespace SFSEMenuFramework::Detail
{
	// Event types, RAII listener ownership, and priority-ordered dispatch are
	// adapted from SKSE Menu Framework 3 commit 928e01a (GPL-3.0). Listener
	// snapshots, quiescent teardown, and deferred render-thread delivery are
	// Starfield-specific.
	struct EventListener final
	{
		static constexpr std::uint32_t activeMask = 0x80000000U;
		static constexpr std::uint32_t inFlightMask = ~activeMask;

		Model::EventHandle         Handle{ 0 };
		Model::EventCallback       Callback{ nullptr };
		float                      Priority{ 0.0F };
		std::atomic<std::uint32_t> State{ activeMask };
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
		constexpr std::size_t maximumListenerCount = 1024;
		constexpr std::size_t maximumPendingTransitionCount = 1024;
		constexpr std::size_t transitionStorageCount =
			maximumPendingTransitionCount + 1;
		constexpr std::size_t maximumTransitionsPerDrain =
			transitionStorageCount;

		using Listener = Detail::EventListener;
		using ListenerPointer = Detail::EventListenerPointer;
		using SnapshotData = Detail::EventSnapshot;
		using SnapshotPointer = EventManager::Snapshot;

		struct PendingTransition final
		{
			Model::EventType Type{ Model::EventType::kNone };
			SnapshotPointer  Listeners;
		};

		struct Registry final
		{
			std::mutex                    MutationMutex;
			std::atomic<SnapshotPointer>   Published;
			Model::EventHandle             NextHandle{ 1 };
			std::mutex                    TransitionMutex;
			std::array<PendingTransition, transitionStorageCount>
				PendingTransitions{};
			std::size_t                   PendingTransitionHead{ 0 };
			std::size_t                   PendingTransitionCount{ 0 };
			std::atomic_flag              TransitionQueueFullLogged{};
			std::atomic_flag              TransitionBacklogLogged{};
		};

		thread_local Listener* executingListener{};

		[[nodiscard]] Registry* GetRegistry() noexcept
		{
			static auto* registry = new (std::nothrow) Registry();
			return registry;
		}

		[[nodiscard]] bool IsExecutableImageAddress(
			Model::EventCallback a_function) noexcept
		{
			static_assert(sizeof(a_function) == sizeof(const void*));
			const auto address = std::bit_cast<const void*>(a_function);

			MEMORY_BASIC_INFORMATION information{};
			if (::VirtualQuery(address, &information, sizeof(information)) !=
					sizeof(information) ||
				information.State != MEM_COMMIT ||
				information.Type != MEM_IMAGE ||
				(information.Protect & PAGE_GUARD) != 0 ||
				!information.AllocationBase) {
				return false;
			}

			const auto protection = information.Protect & 0xFF;
			return protection == PAGE_EXECUTE ||
			       protection == PAGE_EXECUTE_READ ||
			       protection == PAGE_EXECUTE_READWRITE ||
			       protection == PAGE_EXECUTE_WRITECOPY;
		}

		[[nodiscard]] bool IsSupportedEventType(Model::EventType a_type) noexcept
		{
			return a_type == Model::EventType::kOpenMenu ||
			       a_type == Model::EventType::kCloseMenu ||
			       a_type == Model::EventType::kBeforeRender ||
			       a_type == Model::EventType::kAfterRender;
		}

		void LeaveListener(const ListenerPointer& a_listener) noexcept
		{
			const auto previous =
				a_listener->State.fetch_sub(1, std::memory_order_acq_rel);
			if ((previous & Listener::inFlightMask) == 1 ||
				(previous & Listener::activeMask) == 0) {
				a_listener->State.notify_all();
			}
		}

		[[nodiscard]] bool IsListenerActive(
			const ListenerPointer& a_listener) noexcept
		{
			return a_listener &&
				(a_listener->State.load(std::memory_order_acquire) &
					Listener::activeMask) != 0;
		}

		[[nodiscard]] bool TryEnterListener(
			const ListenerPointer& a_listener) noexcept
		{
			if (!a_listener || !a_listener->Callback) {
				return false;
			}

			auto state = a_listener->State.load(std::memory_order_acquire);
			while ((state & Listener::activeMask) != 0) {
				if ((state & Listener::inFlightMask) ==
					Listener::inFlightMask) {
					return false;
				}
				if (a_listener->State.compare_exchange_weak(
						state,
						state + 1,
						std::memory_order_acq_rel,
						std::memory_order_acquire)) {
					return true;
				}
			}
			return false;
		}

		[[nodiscard]] bool QueueTransitionLocked(
			Registry&              a_registry,
			Model::EventType       a_type,
			const SnapshotPointer& a_snapshot,
			bool                   a_emergencyClose) noexcept
		{
			if (!a_snapshot) {
				return true;
			}
			const bool hasActiveListener = std::any_of(
				a_snapshot->Listeners.begin(),
				a_snapshot->Listeners.end(),
				[](const auto& a_listener) {
					return IsListenerActive(a_listener);
				});
			if (!hasActiveListener) {
				return true;
			}
			const auto capacity = maximumPendingTransitionCount +
				(a_emergencyClose ? 1U : 0U);
			if (a_registry.PendingTransitionCount >= capacity) {
				if (!a_registry.TransitionQueueFullLogged.test_and_set(
						std::memory_order_relaxed)) {
					logger::critical(
						"Lifecycle transition queue reached {} entries",
						a_registry.PendingTransitionCount);
				}
				return false;
			}

			const auto tail =
				(a_registry.PendingTransitionHead +
					a_registry.PendingTransitionCount) %
				transitionStorageCount;
			a_registry.PendingTransitions[tail] =
				PendingTransition{ a_type, a_snapshot };
			++a_registry.PendingTransitionCount;
			return true;
		}

		void SetStateWithoutRegistry(
			std::atomic<bool>& a_state,
			bool               a_open) noexcept
		{
			a_state.store(a_open, std::memory_order_release);
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
			!IsExecutableImageAddress(a_registration->Callback)) {
			return Model::RegistrationResult::InvalidArgument;
		}

		try {
			auto listener = std::make_shared<Listener>();
			listener->Callback = a_registration->Callback;
			listener->Priority = a_registration->Priority;

			auto* registry = GetRegistry();
			if (!registry) {
				return Model::RegistrationResult::OutOfMemory;
			}

			std::scoped_lock lock{ registry->MutationMutex };
			const auto current = registry->Published.load(std::memory_order_acquire);
			auto next = std::make_shared<SnapshotData>();
			if (current) {
				next->Listeners.reserve(current->Listeners.size() + 1);
				for (const auto& registered : current->Listeners) {
					if (IsListenerActive(registered)) {
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
				[](const auto& a_left, const auto& a_right) {
					if (a_left->Priority != a_right->Priority) {
						return a_left->Priority > a_right->Priority;
					}
					return a_left->Handle < a_right->Handle;
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

		const auto snapshot = CaptureSnapshot();
		if (!snapshot) {
			return;
		}

		for (const auto& listener : snapshot->Listeners) {
			if (!listener || listener->Handle != a_handle) {
				continue;
			}

			listener->State.fetch_and(
				Listener::inFlightMask,
				std::memory_order_acq_rel);
			const std::uint32_t allowedInFlight =
				executingListener == listener.get() ? 1U : 0U;
			auto state = listener->State.load(std::memory_order_acquire);
			while ((state & Listener::inFlightMask) > allowedInFlight) {
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
		auto* registry = GetRegistry();
		if (!registry) {
			const bool previous =
				a_state.exchange(a_open, std::memory_order_acq_rel);
			return previous != a_open;
		}

		std::scoped_lock lock{ registry->TransitionMutex };
		if (a_state.load(std::memory_order_acquire) == a_open) {
			return false;
		}

		const auto snapshot =
			registry->Published.load(std::memory_order_acquire);
		if (!QueueTransitionLocked(
				*registry,
				a_open ? Model::EventType::kOpenMenu :
					Model::EventType::kCloseMenu,
				snapshot,
				a_emergencyClose && !a_open)) {
			if (a_emergencyClose && !a_open) {
				logger::critical(
					"Applying an emergency main-window close without lifecycle delivery");
				SetStateWithoutRegistry(a_state, false);
				return true;
			}
			return false;
		}
		SetStateWithoutRegistry(a_state, a_open);
		return true;
	}

	bool EventManager::ToggleMainWindowState(
		std::atomic<bool>& a_state,
		bool*              a_open) noexcept
	{
		if (a_open) {
			*a_open = a_state.load(std::memory_order_acquire);
		}

		auto* registry = GetRegistry();
		if (!registry) {
			bool expected = a_state.load(std::memory_order_acquire);
			while (!a_state.compare_exchange_weak(
				expected,
				!expected,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
			}
			if (a_open) {
				*a_open = !expected;
			}
			return true;
		}

		std::scoped_lock lock{ registry->TransitionMutex };
		const bool next = !a_state.load(std::memory_order_acquire);
		const auto snapshot =
			registry->Published.load(std::memory_order_acquire);
		if (!QueueTransitionLocked(
				*registry,
				next ? Model::EventType::kOpenMenu :
					Model::EventType::kCloseMenu,
				snapshot,
				false)) {
			return false;
		}
		SetStateWithoutRegistry(a_state, next);
		if (a_open) {
			*a_open = next;
		}
		return true;
	}

	EventManager::Snapshot EventManager::CaptureSnapshot() noexcept
	{
		const auto* registry = GetRegistry();
		return registry ?
			registry->Published.load(std::memory_order_acquire) :
			nullptr;
	}

	bool EventManager::BeginFrame(Snapshot& a_snapshot) noexcept
	{
		a_snapshot.reset();
		auto* registry = GetRegistry();
		if (!registry) {
			return true;
		}

		for (std::size_t dispatched = 0;
			dispatched < maximumTransitionsPerDrain;
			++dispatched) {
			PendingTransition transition;
			{
				std::scoped_lock lock{ registry->TransitionMutex };
				if (registry->PendingTransitionCount == 0) {
					registry->TransitionQueueFullLogged.clear(
						std::memory_order_relaxed);
					registry->TransitionBacklogLogged.clear(
						std::memory_order_relaxed);
					a_snapshot =
						registry->Published.load(std::memory_order_acquire);
					return true;
				}
				transition = std::move(
					registry->PendingTransitions[
						registry->PendingTransitionHead]);
				registry->PendingTransitions[
					registry->PendingTransitionHead] = {};
				registry->PendingTransitionHead =
					(registry->PendingTransitionHead + 1) %
					transitionStorageCount;
				--registry->PendingTransitionCount;
			}
			Dispatch(transition.Type, transition.Listeners);
		}

		std::scoped_lock lock{ registry->TransitionMutex };
		if (registry->PendingTransitionCount == 0) {
			registry->TransitionQueueFullLogged.clear(
				std::memory_order_relaxed);
			registry->TransitionBacklogLogged.clear(
				std::memory_order_relaxed);
			a_snapshot =
				registry->Published.load(std::memory_order_acquire);
			return true;
		}
		if (!registry->TransitionBacklogLogged.test_and_set(
				std::memory_order_relaxed)) {
			logger::critical(
				"Lifecycle transitions are oscillating; deferring a frame with {} events pending",
				registry->PendingTransitionCount);
		}
		return false;
	}

	void EventManager::Dispatch(
		Model::EventType a_type,
		const Snapshot&  a_snapshot) noexcept
	{
		if (!IsSupportedEventType(a_type) || !a_snapshot) {
			return;
		}

		for (const auto& listener : a_snapshot->Listeners) {
			if (!TryEnterListener(listener)) {
				continue;
			}

			auto* const previousListener = executingListener;
			executingListener = listener.get();
			listener->Callback(a_type);
			executingListener = previousListener;
			LeaveListener(listener);
		}
	}
}
