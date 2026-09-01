#include "runtime/HudManager.h"

#include "appearance/fonts/ConsumerFontScope.h"
#include "runtime/CallbackRegistry.h"
#include "runtime/ConsumerValidation.h"

#include <new>

// Public names, persistent-frame behavior, and registration-order dispatch
// are adapted from SKSE Menu Framework 3 commit 928e01a (GPL-3.0),
// specifically include/HudManager.h and src/HudManager.cpp. Shared-context
// validation, immutable snapshots, and quiescent unregister are
// Starfield-specific.
namespace SFSEMenuFramework::HudManager
{
	namespace
	{
		using Registry = Detail::CallbackRegistry<
			Model::HudElementRenderFunction,
			Model::HudElementHandle>;

		[[nodiscard]] Registry* GetRegistry() noexcept
		{
			static auto* registry = new (std::nothrow) Registry();
			return registry;
		}
	}

	Model::RegistrationResult Register(
		const Model::HudElementRegistration* a_registration,
		Model::HudElementHandle*              a_handle) noexcept
	{
		if (a_handle) {
			*a_handle = 0;
		}
		if (!a_registration || !a_handle) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (a_registration->StructureSize <
				sizeof(Model::HudElementRegistration) ||
			a_registration->ImGui.StructureSize <
				sizeof(Model::ImGuiLayout)) {
			return Model::RegistrationResult::StructureTooSmall;
		}
		if (a_registration->InterfaceVersion != Model::INTERFACE_VERSION) {
			return Model::RegistrationResult::UnsupportedVersion;
		}
		if (!Detail::HasMatchingImGuiLayout(a_registration->ImGui)) {
			return Model::RegistrationResult::ImGuiMismatch;
		}
		if (!Detail::IsExecutableImageFunction(a_registration->Render)) {
			return Model::RegistrationResult::InvalidArgument;
		}

		auto* registry = GetRegistry();
		return registry ?
			registry->Register(
				a_registration->Render,
				a_registration->UserData,
				a_handle) :
			Model::RegistrationResult::OutOfMemory;
	}

	void Unregister(Model::HudElementHandle a_handle) noexcept
	{
		if (auto* registry = GetRegistry()) {
			registry->Unregister(a_handle);
		}
	}

	void Render(const Model::RenderContext& a_context) noexcept
	{
		if (auto* registry = GetRegistry()) {
			registry->Dispatch([&a_context](const auto& a_entry) noexcept {
				ConsumerFontScope::CallbackScope callbackScope{ "HUD" };
				a_entry.Function(&a_context, a_entry.UserData);
			});
		}
	}
}
