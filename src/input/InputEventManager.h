#pragma once

#include <SFSEMenuFramework/API.h>

namespace SFSEMenuFramework::InputEventManager
{
	[[nodiscard]] Model::RegistrationResult Register(
		const Model::InputEventRegistration* a_registration,
		Model::InputEventHandle*              a_handle) noexcept;
	void Unregister(Model::InputEventHandle a_handle) noexcept;

	[[nodiscard]] bool IsDispatchEnabled() noexcept;
	[[nodiscard]] bool Dispatch(RE::InputEvent* a_event) noexcept;
	void SetImGuiItemActive(bool a_active) noexcept;
}
