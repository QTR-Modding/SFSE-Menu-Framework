#pragma once

#include "appearance/ThemeImage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace SFSEMenuFramework::ThemeManager
{
	struct ThemeEntry final
	{
		std::string           Name;
		std::filesystem::path Path;
	};

	void Initialize();
	void ApplyPending() noexcept;

	[[nodiscard]] std::span<const ThemeEntry> GetThemes() noexcept;
	[[nodiscard]] std::size_t GetSelectedThemeIndex() noexcept;
	[[nodiscard]] bool QueueTheme(std::size_t);
	[[nodiscard]] bool QueueConfiguredTheme();
	[[nodiscard]] bool QueueUIScale(float) noexcept;
	[[nodiscard]] bool QueueBackgroundOpacity(float) noexcept;
	[[nodiscard]] bool QueueWallpaperOpacity(float) noexcept;
	[[nodiscard]] bool QueueWallpaperDimming(float) noexcept;
	[[nodiscard]] bool IsWallpaperSelected() noexcept;
	[[nodiscard]] std::shared_ptr<const ThemeImage> GetWallpaperImage() noexcept;
	void SetWallpaperTexture(std::uintptr_t a_texture, bool a_failed) noexcept;
	[[nodiscard]] bool HasWallpaperUploadError() noexcept;
	void RenderCurrentWindowBackdrop() noexcept;
}
