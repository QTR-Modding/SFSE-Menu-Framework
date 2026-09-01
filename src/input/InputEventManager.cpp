#include "input/InputEventManager.h"

#include "runtime/CallbackRegistry.h"
#include "runtime/ConsumerValidation.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <new>

// Public names, per-event callback ordering, and all-listener consumption are
// adapted from SKSE Menu Framework 3 commit 928e01a (GPL-3.0), specifically
// include/InputEventHandler.h and src/InputEventHandler.cpp. Immutable
// snapshots, quiescent unregister, and the render-thread activity gate are
// Starfield-specific.
namespace SFSEMenuFramework::InputEventManager
{
	namespace
	{
		using Registry = Detail::CallbackRegistry<
			Model::InputEventCallback,
			Model::InputEventHandle>;

		constexpr std::uint64_t maximumActivityFrameAgeMilliseconds = 250;
		std::atomic<std::uint64_t> activeItemFrameTick{ 0 };

		[[nodiscard]] Registry* GetRegistry() noexcept
		{
			static auto* registry = new (std::nothrow) Registry();
			return registry;
		}
	}

	Model::RegistrationResult Register(
		const Model::InputEventRegistration* a_registration,
		Model::InputEventHandle*              a_handle) noexcept
	{
		if (a_handle) {
			*a_handle = 0;
		}
		if (!a_registration || !a_handle) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (a_registration->StructureSize <
			sizeof(Model::InputEventRegistration)) {
			return Model::RegistrationResult::StructureTooSmall;
		}
		if (a_registration->InterfaceVersion != Model::INTERFACE_VERSION) {
			return Model::RegistrationResult::UnsupportedVersion;
		}
		if (!Detail::IsExecutableImageFunction(a_registration->Callback)) {
			return Model::RegistrationResult::InvalidArgument;
		}

		auto* registry = GetRegistry();
		return registry ?
			registry->Register(a_registration->Callback, nullptr, a_handle) :
			Model::RegistrationResult::OutOfMemory;
	}

	void Unregister(Model::InputEventHandle a_handle) noexcept
	{
		if (auto* registry = GetRegistry()) {
			registry->Unregister(a_handle);
		}
	}

	bool IsDispatchEnabled() noexcept
	{
		const auto activeTick =
			activeItemFrameTick.load(std::memory_order_acquire);
		return activeTick == 0 ||
		       (::GetTickCount64() - activeTick) >
			       maximumActivityFrameAgeMilliseconds;
	}

	bool Dispatch(RE::InputEvent* a_event) noexcept
	{
		if (!a_event) {
			return false;
		}

		// Starfield's input processor invokes this registry serially and without
		// re-entering it. CallbackRegistry's self-unregister allowance relies on
		// that verified hook contract.
		auto* registry = GetRegistry();
		if (!registry) {
			return false;
		}

		bool consumed{};
		registry->Dispatch([a_event, &consumed](const auto& a_entry) noexcept {
			if (a_entry.Function(a_event)) {
				consumed = true;
			}
		});
		return consumed;
	}

	void SetImGuiItemActive(bool a_active) noexcept
	{
		activeItemFrameTick.store(
			a_active ? ::GetTickCount64() : 0,
			std::memory_order_release);
	}
}
