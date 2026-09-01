#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <span>
#include <string>

namespace SFSEMenuFramework::ThemeManager
{
	struct ThemeEntry final
	{
		std::string           Name;
		std::filesystem::path Path;
	};

	inline constexpr std::size_t NO_THEME =
		(std::numeric_limits<std::size_t>::max)();

	void Initialize();
	void ApplyPending() noexcept;

	[[nodiscard]] std::span<const ThemeEntry> GetThemes() noexcept;
	[[nodiscard]] std::size_t GetSelectedThemeIndex() noexcept;
	[[nodiscard]] bool QueueTheme(std::size_t a_index);
	[[nodiscard]] bool QueueConfiguredTheme();
	[[nodiscard]] bool QueueUIScale(float a_scale) noexcept;
}
