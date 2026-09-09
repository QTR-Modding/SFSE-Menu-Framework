#include "appearance/GamepadIcons.h"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <system_error>

namespace SFSEMenuFramework::GamepadIcons
{
	namespace
	{
		// Icon names, paths and all-or-nothing availability follow SKSE-MF
		// GamepadNavigation.cpp at 8fb2d295aee582a24204b015f39214ad43717728 (GPL-3.0).
		struct IconAsset
		{
			std::shared_ptr<const ThemeImage> Image;
			ImTextureID Texture{};
		};

		std::array<IconAsset, count> assets;
		bool initialized{};
		bool usesPlayStation{};
		bool missingIconsLogged{};
		const std::filesystem::path iconDirectory{ "Data/Interface/ImGuiIcons/Icons" };

		const char* IconName(Slot a_slot, bool a_playStation)
		{
			switch (a_slot) {
			case Slot::Up:
				return "Up";
			case Slot::Down:
				return "Down";
			case Slot::Left:
				return "Left";
			case Slot::Right:
				return "Right";
			case Slot::Confirm:
				return a_playStation ? "PS3_A" : "360_A";
			case Slot::Cancel:
				return a_playStation ? "PS3_B" : "360_B";
			case Slot::Options:
				return a_playStation ? "PS3_X" : "360_X";
			case Slot::RightShoulder:
				return a_playStation ? "PS3_RB" : "360_RB";
			case Slot::RightStick:
				return a_playStation ? "PS3_R3" : "360_RS";
			case Slot::Count:
				return "UnknownKey";
			}
			return "UnknownKey";
		}
	}

	void Update(bool a_playStation)
	{
		if (initialized && usesPlayStation == a_playStation) {
			return;
		}
		initialized = true;
		usesPlayStation = a_playStation;

		for (std::size_t index = 0; index < assets.size(); ++index) {
			auto& asset = assets[index];
			asset = {};
			const auto name = std::format("{}.png", IconName(static_cast<Slot>(index), a_playStation));
			const auto path = iconDirectory / name;
			std::error_code error;
			if (!std::filesystem::exists(path, error) || error) {
				if (!missingIconsLogged) {
					logger::error("ImGui Icons is required for gamepad prompts; missing '{}'", path.string());
					missingIconsLogged = true;
				}
				continue;
			}

			asset.Image = LoadThemeImage(iconDirectory, name);
			if (!asset.Image && !missingIconsLogged) {
				logger::error("Could not load required ImGui Icons texture '{}'", path.string());
				missingIconsLogged = true;
			}
		}
	}

	std::shared_ptr<const ThemeImage> GetImage(std::size_t a_index) noexcept
	{
		return a_index < assets.size() ? assets[a_index].Image : nullptr;
	}

	void SetTexture(std::size_t a_index, std::uintptr_t a_texture) noexcept
	{
		if (a_index < assets.size()) {
			assets[a_index].Texture = reinterpret_cast<ImTextureID>(a_texture);
		}
	}

	ImTextureID GetTexture(Slot a_slot) noexcept
	{
		const auto index = static_cast<std::size_t>(a_slot);
		return index < assets.size() ? assets[index].Texture : nullptr;
	}

	bool IsAvailable() noexcept
	{
		return initialized && std::ranges::all_of(assets, [](const auto& asset) {
			return asset.Texture != nullptr;
		});
	}
}
