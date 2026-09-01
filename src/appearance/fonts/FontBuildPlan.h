#pragma once

#include "appearance/fonts/FontCatalog.h"
#include "config/FrameworkSettings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SFSEMenuFramework::Fonts
{
	inline constexpr std::string_view preferredFallbackFont{
		"Jost-500-Medium.ttf"
	};
	inline constexpr std::string_view secondaryFallbackFont{
		"Jost-400-Book.ttf"
	};

	enum class PrimaryMode : std::uint8_t
	{
		Automatic,
		Exact,
		Embedded
	};

	struct FontBuildOptions final
	{
		PrimaryMode      Primary{ PrimaryMode::Automatic };
		std::string_view ExactPrimary;
		bool             IncludeNamedFonts{ true };
		bool             IncludeIcons{ true };
		bool             IncludeTextFallback{ true };
		std::string_view FallbackReason;
	};

	struct FontBuildPlan final
	{
		std::vector<FontAsset> Assets;
		std::vector<std::size_t> NamedAssets;
		std::optional<std::size_t> PrimaryAsset;
		std::optional<std::size_t> TextFallbackAsset;
		std::array<std::optional<std::size_t>, 3> IconAssets;
		std::optional<FontWeightAxis> PrimaryWeightAxis;
		float DefaultRasterSize{};
		std::string ActiveName;
		std::string FallbackReason;
	};

	[[nodiscard]] float ConfiguredRasterSize(
		const FrameworkSettings::FontSettings&) noexcept;
	[[nodiscard]] float AssetRasterSize(
		const FontAsset&,
		const FrameworkSettings::FontSettings&) noexcept;

	[[nodiscard]] bool PrepareFontBuildPlan(
		std::span<FontEntry>,
		const FrameworkSettings::FontSettings&,
		const FontBuildOptions&,
		FontBuildPlan&);
}
