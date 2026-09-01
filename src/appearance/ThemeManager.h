#pragma once

#include <cstddef>
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
}
