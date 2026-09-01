#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SFSEMenuFramework::Fonts
{
	struct FontWeightAxis final
	{
		float Minimum{};
		float Default{};
		float Maximum{};
	};

	struct FontEntry final
	{
		std::string                   Name;
		std::filesystem::path         Path;
		std::optional<FontWeightAxis> WeightAxis;
		bool                          WeightAxisInspected{};
	};

	struct FontAsset final
	{
		std::string                   Name;
		std::vector<std::uint8_t>     Bytes;
		std::optional<FontWeightAxis> WeightAxis;
		std::optional<float>          SizeOverride;
		bool                          HasPrintableAscii{};
	};

	enum class IconStyle : std::uint8_t
	{
		Solid,
		Regular,
		Brands
	};

	void Refresh(std::vector<FontEntry>&);
	[[nodiscard]] FontEntry* FindExact(
		std::span<FontEntry>, std::string_view) noexcept;
	[[nodiscard]] bool Load(FontEntry&, FontAsset&);
	[[nodiscard]] bool LoadIconAsset(IconStyle, FontAsset&);
	[[nodiscard]] bool Inspect(FontEntry&);
	[[nodiscard]] std::string_view GetIconFileName(IconStyle) noexcept;
	[[nodiscard]] bool IsIconFontName(std::string_view) noexcept;
}
