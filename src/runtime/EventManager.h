#pragma once

#include "runtime/CallbackRegistry.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace SFSEMenuFramework
{
	namespace Detail
	{
		using EventCallbacks = CallbackRegistry<Model::EventCallback, Model::EventHandle>;
	}

	class EventManager final
	{
	public:
		using Snapshot = Detail::EventCallbacks::Snapshot;

		[[nodiscard]] static Model::EventHandle Register(
			Model::EventCallback a_callback,
			float                a_priority) noexcept;
		static void Unregister(Model::EventHandle a_handle) noexcept;
		[[nodiscard]] static bool SetMainWindowState(
			std::atomic<bool>& a_state, bool a_open,
			bool               a_emergencyClose = false) noexcept;
		[[nodiscard]] static bool BeginFrame(Snapshot& a_snapshot) noexcept;
		static void Dispatch(
			Model::EventType a_type, const Snapshot& a_snapshot) noexcept;
	};
}
