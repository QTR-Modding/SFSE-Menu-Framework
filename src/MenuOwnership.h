#pragma once

#include <cstdint>

namespace SFSEMenuFramework::MenuOwnership
{
	enum class HostAvailability : std::uint8_t
	{
		Interactive,
		Transient,
		Unavailable
	};

	enum class InputDisposition : std::uint8_t
	{
		PassThrough,
		Suppress,
		RouteToMenu
	};

	struct ReconcileContext final
	{
		HostAvailability Availability{ HostAvailability::Unavailable };
		bool             PauseAllowed{ true };
	};

	void Install();

	// Engine ownership mutations must run on the Starfield HWND thread. Other
	// threads request this reconciliation through the Win32 platform bridge.
	void ReconcileOnHostWindowThread(const ReconcileContext& a_context);
	void ReleaseOnHostWindowThread();

	[[nodiscard]] InputDisposition GetInputDisposition() noexcept;
}
