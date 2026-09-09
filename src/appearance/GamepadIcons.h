#pragma once

#include "appearance/ThemeImage.h"

#include <imgui.h>

#include <cstddef>

namespace SFSEMenuFramework::GamepadIcons
{
	enum class Slot
	{
		Up, Down, Left, Right, Confirm, Cancel, Options, RightShoulder, RightStick, Count
	};
	inline constexpr std::size_t count = static_cast<std::size_t>(Slot::Count);

	// Render-thread only. Texture IDs belong to the renderer's current frame heap.
	void Update(bool a_playStation);
	[[nodiscard]] std::shared_ptr<const ThemeImage> GetImage(std::size_t a_index) noexcept;
	void SetTexture(std::size_t a_index, std::uintptr_t a_texture) noexcept;
	[[nodiscard]] ImTextureID GetTexture(Slot a_slot) noexcept;
	[[nodiscard]] bool IsAvailable() noexcept;
}
