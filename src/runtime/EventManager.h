#pragma once

#include "api/InternalTypes.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace SFSEMenuFramework
{
	namespace Detail
	{
		struct EventSnapshot;
	}

	class EventManager final
	{
	public:
		using Snapshot = std::shared_ptr<const Detail::EventSnapshot>;

		[[nodiscard]] static Model::RegistrationResult Register(
			const Model::EventRegistration* a_registration,
			Model::EventHandle*              a_handle) noexcept;
		static void Unregister(Model::EventHandle a_handle) noexcept;
		[[nodiscard]] static bool SetMainWindowState(
			std::atomic<bool>& a_state, bool a_open,
			bool               a_emergencyClose = false) noexcept;
		[[nodiscard]] static bool BeginFrame(Snapshot& a_snapshot) noexcept;
		static void Dispatch(
			Model::EventType a_type, const Snapshot& a_snapshot) noexcept;
	};
}
