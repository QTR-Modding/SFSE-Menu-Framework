#pragma once

#include <utility>

namespace SFSEMenuFramework::RenderHooks::StreamlineDiagnostic
{
	[[nodiscard]] bool EnsureInstalled() noexcept;
	void ResetRegion() noexcept;
	void ActivateRegion() noexcept;
}
