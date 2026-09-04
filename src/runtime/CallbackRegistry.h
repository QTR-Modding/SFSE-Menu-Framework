#pragma once

#include "api/InternalTypes.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

namespace SFSEMenuFramework::Detail
{
	template <class Callback, class Handle, std::size_t MaximumEntries = 1024>
	class CallbackRegistry final
	{
	public:
		struct Entry final
		{
			static constexpr std::uint32_t ACTIVE = 0x80000000U;
			static constexpr std::uint32_t IN_FLIGHT = ~ACTIVE;

			Handle                     RegistrationHandle{};
			Callback                   Function{ nullptr };
			float                      Priority{ 0.0F };
			std::atomic<std::uint32_t> State{ ACTIVE };

			[[nodiscard]] bool IsActive() const noexcept
			{
				return (State.load(std::memory_order_acquire) & ACTIVE) != 0;
			}

			[[nodiscard]] bool Enter() noexcept
			{
				auto state = State.load(std::memory_order_acquire);
				while ((state & ACTIVE) != 0 &&
					(state & IN_FLIGHT) != IN_FLIGHT) {
					if (State.compare_exchange_weak(
							state,
							state + 1,
							std::memory_order_acq_rel,
							std::memory_order_acquire)) {
						return true;
					}
				}
				return false;
			}

			void Leave() noexcept
			{
				const auto previous =
					State.fetch_sub(1, std::memory_order_acq_rel);
				if ((previous & IN_FLIGHT) == 1 ||
					(previous & ACTIVE) == 0) {
					State.notify_all();
				}
			}
		};

		using Entries = std::vector<std::shared_ptr<Entry>>;
		using Snapshot = std::shared_ptr<const Entries>;

		[[nodiscard]] Handle Register(
			Callback a_callback,
			float a_priority = 0.0F,
			std::mutex* a_publicationMutex = nullptr) noexcept
		{
			if (!a_callback) {
				return 0;
			}

			// Registration crosses a noexcept DLL boundary. Allocation and mutex
			// failures are reported to clients as an invalid handle.
			try {
				auto entry = std::make_shared<Entry>();
				entry->Function = a_callback;
				entry->Priority = a_priority;

				std::scoped_lock lock{ MutationMutex };
				const auto current = Published.load(std::memory_order_acquire);
				auto next = std::make_shared<Entries>();
				if (current) {
					next->reserve(current->size() + 1);
					for (const auto& registered : *current) {
						if (registered && registered->IsActive()) {
							next->push_back(registered);
						}
					}
				}
				if (next->size() >= MaximumEntries || NextHandle == 0) {
					return 0;
				}

				entry->RegistrationHandle = NextHandle++;
				const auto handle = entry->RegistrationHandle;
				// Insert after equal priorities to retain registration order.
				const auto position = std::upper_bound(
					next->begin(), next->end(), a_priority,
					[](float a_priorityValue, const auto& a_entry) {
						return a_priorityValue > a_entry->Priority;
					});
				next->insert(position, std::move(entry));
				// Lifecycle edges and listener publication share this lock.
				std::unique_lock<std::mutex> publicationLock;
				if (a_publicationMutex) {
					publicationLock = std::unique_lock{ *a_publicationMutex };
				}
				Published.store(std::move(next), std::memory_order_release);
				return handle;
			} catch (const std::bad_alloc&) {
				return 0;
			} catch (const std::system_error&) {
				return 0;
			}
		}

		void Unregister(Handle a_handle) noexcept
		{
			if (a_handle == 0) {
				return;
			}

			const auto snapshot = Published.load(std::memory_order_acquire);
			if (!snapshot) {
				return;
			}
			for (const auto& entry : *snapshot) {
				if (!entry ||
					entry->RegistrationHandle != a_handle) {
					continue;
				}

				entry->State.fetch_and(Entry::IN_FLIGHT, std::memory_order_acq_rel);
				const std::uint32_t allowedInFlight =
					ExecutingEntry == entry.get() ? 1U : 0U;
				auto state = entry->State.load(std::memory_order_acquire);
				while ((state & Entry::IN_FLIGHT) > allowedInFlight) {
					entry->State.wait(state, std::memory_order_acquire);
					state = entry->State.load(std::memory_order_acquire);
				}
				return;
			}
		}

		[[nodiscard]] Snapshot CaptureSnapshot() const noexcept
		{
			return Published.load(std::memory_order_acquire);
		}

		template <class Visitor>
		void Dispatch(Visitor&& a_visitor) noexcept
		{
			Dispatch(CaptureSnapshot(), std::forward<Visitor>(a_visitor));
		}

		template <class Visitor>
		static void Dispatch(const Snapshot& a_snapshot, Visitor&& a_visitor) noexcept
		{
			if (!a_snapshot) {
				return;
			}
			for (const auto& entry : *a_snapshot) {
				if (!entry || !entry->Function || !entry->Enter()) {
					continue;
				}

				auto* const previousEntry = ExecutingEntry;
				ExecutingEntry = entry.get();
				a_visitor(*entry);
				ExecutingEntry = previousEntry;
				entry->Leave();
			}
		}

	private:
		std::mutex                                  MutationMutex;
		std::atomic<Snapshot>                        Published{};
		Handle                                      NextHandle{ 1 };
		inline static thread_local Entry*            ExecutingEntry{};
	};
}
