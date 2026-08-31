#pragma once

#include <string_view>

namespace SFSEMenuFramework::RootMenuConfig
{
	[[nodiscard]] bool Load() noexcept;
	[[nodiscard]] bool IsFavorite(std::string_view a_menuName) noexcept;
	[[nodiscard]] bool IsArchived(std::string_view a_menuName) noexcept;
	[[nodiscard]] bool SetFavorite(
		std::string_view a_menuName,
		bool             a_favorite) noexcept;
	[[nodiscard]] bool SetArchived(
		std::string_view a_menuName,
		bool             a_archived) noexcept;
}
