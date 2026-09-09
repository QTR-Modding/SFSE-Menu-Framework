#pragma once

#include "appearance/ThemeImage.h"

#include <span>
#include <string>

namespace SFSEMenuFramework::CursorManager
{
	struct Entry final
	{
		std::string Name;
		std::filesystem::path Path;
	};

	void Refresh();
	void Update();
	[[nodiscard]] std::span<const Entry> GetCursors() noexcept;
	[[nodiscard]] std::shared_ptr<const ThemeImage> GetImage() noexcept;
	void SetTexture(std::uintptr_t a_texture, bool a_failed) noexcept;
	[[nodiscard]] bool HasError() noexcept;
	[[nodiscard]] bool Render();
}
