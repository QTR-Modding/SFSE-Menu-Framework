#pragma once

#include "api/InternalTypes.h"

namespace SFSEMenuFramework::HudManager
{
	[[nodiscard]] Model::HudElementHandle Register(
		Model::ClientHudElementRenderFunction a_callback) noexcept;
	void Unregister(Model::HudElementHandle a_handle) noexcept;

	void Render() noexcept;
}
