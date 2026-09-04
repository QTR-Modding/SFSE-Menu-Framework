#pragma once

#include "api/InternalTypes.h"

namespace SFSEMenuFramework::InputEventManager
{
	[[nodiscard]] Model::InputEventHandle Register(
		Model::InputEventCallback a_callback) noexcept;
	void Unregister(Model::InputEventHandle a_handle) noexcept;

	[[nodiscard]] bool IsDispatchEnabled() noexcept;
	[[nodiscard]] bool Dispatch(RE::InputEvent* a_event) noexcept;
	void SetImGuiItemActive(bool a_active) noexcept;
}
