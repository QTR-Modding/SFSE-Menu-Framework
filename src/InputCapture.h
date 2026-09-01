#pragma once

#include <cstdint>

namespace SFSEMenuFramework::InputCapture
{
	enum class KeyboardEdgeMatch : std::uint8_t
	{
		InitialPress,
		HeldPress
	};

	[[nodiscard]] bool Install();
	void ArmFunctionalCapture() noexcept;

	[[nodiscard]] bool RequestKeyboardSuppression(
		std::int32_t      a_expectedEventID,
		KeyboardEdgeMatch a_match) noexcept;
	void CancelPendingHeldKeyboardSuppression() noexcept;
	void CancelPendingKeyboardSuppression() noexcept;
	void SetModal(bool a_modal) noexcept;
	[[nodiscard]] bool IsModal() noexcept;
	[[nodiscard]] bool IsOperational() noexcept;
}
