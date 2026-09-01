#include "config/RootMenuConfig.h"

#include <Windows.h>

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
		constexpr char temporaryConfigPath[]{
			"Data/SFSE/Plugins/SFSEMenuFrameworkMenuConfig.json.tmp"
		};

		using MenuNames = std::set<std::string, std::less<>>;
		enum class MenuList
		{
			Favorites,
			Archived
		};

		MenuNames favoriteMenus;
		MenuNames archivedMenus;

		void DiscardTemporaryConfig() noexcept
		{
			static_cast<void>(::DeleteFileA(temporaryConfigPath));
		}

		[[nodiscard]] bool FlushTemporaryConfig() noexcept
		{
			const auto file = ::CreateFileA(
				temporaryConfigPath,
				GENERIC_WRITE,
				FILE_SHARE_READ,
				nullptr,
				OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				logger::error(
					"Could not reopen temporary root menu configuration '{}' (Windows error {})",
					temporaryConfigPath,
					::GetLastError());
				return false;
			}

			bool success = ::FlushFileBuffers(file) != FALSE;
			auto error = success ? ERROR_SUCCESS : ::GetLastError();
			if (!::CloseHandle(file) && success) {
				success = false;
				error = ::GetLastError();
			}
			if (!success) {
				logger::error(
					"Could not flush temporary root menu configuration '{}' (Windows error {})",
					temporaryConfigPath,
					error);
			}
			return success;
		}

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

		[[nodiscard]] bool Save(
			const MenuNames& a_favoriteMenus,
			const MenuNames& a_archivedMenus) noexcept
		{
			try {
				const nlohmann::json config{
					{ "favorites", a_favoriteMenus },
					{ "archived", a_archivedMenus }
				};
				const std::string serializedConfig = config.dump(2) + '\n';

				std::ofstream file{ temporaryConfigPath, std::ios::trunc };
				if (!file.good()) {
					DiscardTemporaryConfig();
					logger::error(
						"Could not open temporary root menu configuration '{}'",
						temporaryConfigPath);
					return false;
				}

				file << serializedConfig;
				file.flush();
				const bool written = file.good();
				file.close();
				if (!written || file.fail()) {
					DiscardTemporaryConfig();
					logger::error(
						"Could not finish writing temporary root menu configuration '{}'",
						temporaryConfigPath);
					return false;
				}
				if (!FlushTemporaryConfig()) {
					DiscardTemporaryConfig();
					return false;
				}
				if (!::MoveFileExA(
						temporaryConfigPath,
						configPath,
						MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
					const auto error = ::GetLastError();
					DiscardTemporaryConfig();
					logger::error(
						"Could not replace root menu configuration '{}' (Windows error {})",
						configPath,
						error);
					return false;
				}
				return true;
			} catch (const std::exception& exception) {
				DiscardTemporaryConfig();
				logger::error(
					"Could not serialize root menu configuration '{}': {}",
					configPath, exception.what());
				return false;
			}
		}

		[[nodiscard]] bool SetMenuState(
			MenuNames&       a_menuNames,
			std::string_view a_menuName,
			bool             a_enabled,
			MenuList         a_list) noexcept
		{
			try {
				MenuNames candidate = a_menuNames;
				const bool changed = a_enabled ?
					candidate.emplace(a_menuName).second :
					candidate.erase(a_menuName) > 0;
				if (!changed) {
					return true;
				}
				const bool saved = a_list == MenuList::Favorites ?
					Save(candidate, archivedMenus) :
					Save(favoriteMenus, candidate);
				if (saved) {
					a_menuNames.swap(candidate);
				}
				return saved;
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
		return SetMenuState(
			favoriteMenus, a_menuName, a_favorite, MenuList::Favorites);
	}
	bool SetArchived(std::string_view a_menuName, bool a_archived) noexcept
	{
		return SetMenuState(
			archivedMenus, a_menuName, a_archived, MenuList::Archived);
	}
}
