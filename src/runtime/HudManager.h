#pragma once

#include <SFSEMenuFramework/API.h>

namespace SFSEMenuFramework::HudManager
{
	[[nodiscard]] Model::RegistrationResult Register(
		const Model::HudElementRegistration* a_registration,
		Model::HudElementHandle*              a_handle) noexcept;
	void Unregister(Model::HudElementHandle a_handle) noexcept;

	void Render(const Model::RenderContext& a_context) noexcept;
}
