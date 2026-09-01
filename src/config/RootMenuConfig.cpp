#include "config/RootMenuConfig.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace SFSEMenuFramework::RootMenuConfig
{
	namespace
	{
		// Directly adapted from SKSE Menu Framework 3 RootMenuConfig.cpp at
		// commit 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// The path and namespace are changed for SFSE; bounded error handling and
		// save-result propagation are Starfield-port corrections.
		constexpr char configPath[]{ "Data/SFSE/Plugins/SFSEMenuFrameworkMenuConfig.json" };

		using MenuNames = std::set<std::string, std::less<>>;
		MenuNames favoriteMenus;
		MenuNames archivedMenus;

		void LoadMenuNames(
			const nlohmann::json& a_config, const char* a_key, MenuNames& a_menuNames)
		{
			if (!a_config.contains(a_key) || !a_config[a_key].is_array()) {
				return;
			}

			for (const auto& value : a_config[a_key]) {
				if (value.is_string()) {
					a_menuNames.insert(value.get<std::string>());
				}
			}
		}

		[[nodiscard]] bool Save() noexcept
		{
			try {
				const nlohmann::json config{
					{ "favorites", favoriteMenus },
					{ "archived", archivedMenus }
				};

				std::ofstream file{ configPath, std::ios::trunc };
				if (!file.good()) {
					logger::error("Could not save root menu configuration to '{}'", configPath);
					return false;
				}

				file << config.dump(2) << '\n';
				file.flush();
				const bool written = file.good();
				file.close();
				if (!written || file.fail()) {
					logger::error("Could not finish writing root menu configuration to '{}'", configPath);
					return false;
				}
				return true;
			} catch (const std::exception& exception) {
				logger::error(
					"Could not serialize root menu configuration '{}': {}",
					configPath, exception.what());
				return false;
			}
		}

		[[nodiscard]] bool SetMenuState(
			MenuNames& a_menuNames, std::string_view a_menuName, bool a_enabled) noexcept
		{
			try {
				const bool changed = a_enabled ?
					a_menuNames.emplace(a_menuName).second :
					a_menuNames.erase(a_menuName) > 0;
				return !changed || Save();
			} catch (const std::exception& exception) {
				logger::error(
					"Could not update root menu configuration '{}': {}",
					configPath, exception.what());
				return false;
			}
		}
	}

	bool Load() noexcept
	{
		favoriteMenus.clear();
		archivedMenus.clear();

		std::error_code pathError;
		if (!std::filesystem::exists(configPath, pathError)) {
			if (pathError) {
				logger::error(
					"Could not inspect root menu configuration '{}': {}",
					configPath, pathError.message());
				return false;
			}
			return true;
		}

		std::ifstream file{ configPath };
		if (!file.is_open()) {
			logger::error("Could not open root menu configuration '{}' for reading", configPath);
			return false;
		}

		try {
			const auto config = nlohmann::json::parse(file);
			if (!config.is_object()) {
				logger::warn("Root menu configuration '{}' must contain a JSON object", configPath);
				return false;
			}

			LoadMenuNames(config, "favorites", favoriteMenus);
			LoadMenuNames(config, "archived", archivedMenus);
			// Preserve the compatibility alias accepted by the pinned source.
			LoadMenuNames(config, "hidden", archivedMenus);
			return true;
		} catch (const std::exception& exception) {
			logger::error(
				"Could not read root menu configuration '{}': {}",
				configPath, exception.what());
			return false;
		}
	}

	bool IsFavorite(std::string_view a_menuName) noexcept { return favoriteMenus.contains(a_menuName); }
	bool IsArchived(std::string_view a_menuName) noexcept { return archivedMenus.contains(a_menuName); }
	bool SetFavorite(std::string_view a_menuName, bool a_favorite) noexcept
	{
		return SetMenuState(favoriteMenus, a_menuName, a_favorite);
	}
	bool SetArchived(std::string_view a_menuName, bool a_archived) noexcept
	{
		return SetMenuState(archivedMenus, a_menuName, a_archived);
	}
}
