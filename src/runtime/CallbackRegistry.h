#pragma once

#include "api/InternalTypes.h"

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

		[[nodiscard]] Handle Register(Callback a_callback) noexcept
		{
			if (!a_callback) {
				return 0;
			}

			// Registration crosses a noexcept DLL boundary. Allocation and mutex
			// failures are reported to clients as an invalid handle.
			try {
				auto entry = std::make_shared<Entry>();
				entry->Function = a_callback;

				std::scoped_lock lock{ MutationMutex };
				const auto current = Published.load(std::memory_order_acquire);
				auto next = std::make_shared<Snapshot>();
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
				next->push_back(std::move(entry));
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

		template <class Visitor>
		void Dispatch(Visitor&& a_visitor) noexcept
		{
			const auto snapshot = Published.load(std::memory_order_acquire);
			if (!snapshot) {
				return;
			}
			for (const auto& entry : *snapshot) {
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
		using EntryPointer = std::shared_ptr<Entry>;
		using Snapshot = std::vector<EntryPointer>;

		std::mutex                                  MutationMutex;
		std::atomic<std::shared_ptr<const Snapshot>> Published{};
		Handle                                      NextHandle{ 1 };
		inline static thread_local Entry*            ExecutingEntry{};
	};
}
