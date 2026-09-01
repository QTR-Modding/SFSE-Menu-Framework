#pragma once

#include "appearance/fonts/FontCatalog.h"
#include "config/FrameworkSettings.h"

#include <imgui.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SFSEMenuFramework::Fonts
{
	struct FontAlias final
	{
		std::string Name;
		ImFont*     Font{};
	};

	struct AtlasGeneration final
	{
		AtlasGeneration() = default;
		AtlasGeneration(const AtlasGeneration&) = delete;
		AtlasGeneration(AtlasGeneration&&) noexcept = default;
		AtlasGeneration& operator=(const AtlasGeneration&) = delete;
		AtlasGeneration& operator=(AtlasGeneration&&) noexcept = default;

		FrameworkSettings::FontSettings Settings{};
		std::optional<FontWeightAxis>    WeightAxis;
		std::vector<ImWchar>             TextGlyphRanges;
		std::vector<FontAlias>           Aliases;
		ImFont*                          DefaultFont{};
		float                            DefaultRasterSize{};
		std::string                      ActiveName;
		std::string                      FallbackReason;

		[[nodiscard]] float RasterSize() const noexcept;
		[[nodiscard]] ImFont* Find(std::string_view) const noexcept;
	};

	[[nodiscard]] bool BuildAtlas(
		ImFontAtlas&,
		std::span<FontEntry>,
		const FrameworkSettings::FontSettings&,
		AtlasGeneration&);
}
