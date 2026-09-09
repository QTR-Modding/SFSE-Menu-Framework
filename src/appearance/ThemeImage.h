#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace SFSEMenuFramework
{
	struct ThemeImage final
	{
		std::uint32_t Width{};
		std::uint32_t Height{};
		std::vector<unsigned char> Pixels;
	};

	// Paths are relative to the theme JSON, within the theme directory.
	[[nodiscard]] std::shared_ptr<const ThemeImage> LoadThemeImage(
		const std::filesystem::path& a_themeDirectory, std::string_view a_relativePath,
		std::uint32_t a_maximumDimension = 4096);
}
