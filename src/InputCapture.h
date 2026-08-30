#pragma once

namespace SFSEMenuFramework::InputCapture
{
	[[nodiscard]] bool Install();

	void               FlushDiagnostics() noexcept;
	void               SetModal(bool a_modal) noexcept;
	[[nodiscard]] bool IsModal() noexcept;
	[[nodiscard]] bool IsOperational() noexcept;
}
