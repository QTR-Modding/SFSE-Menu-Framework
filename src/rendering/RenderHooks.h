#pragma once

namespace SFSEMenuFramework::RenderHooks
{
	[[nodiscard]] bool Install();
	[[nodiscard]] bool HasTerminalRendererFailure() noexcept;
}
