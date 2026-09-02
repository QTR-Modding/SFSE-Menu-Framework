#pragma once

#include "appearance/fonts/FontCatalog.h"
#include "config/FrameworkSettings.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

struct ImGuiIO;

namespace SFSEMenuFramework::FontManager
{
	using FontWeightAxis = Fonts::FontWeightAxis;
	using FontEntry = Fonts::FontEntry;

	struct ActiveFontInfo final
	{
		FrameworkSettings::FontSettings Settings;
		std::string_view Name;
		std::optional<FontWeightAxis>    WeightAxis;
		std::string_view FallbackReason;
		float RasterSize{};
	};

	struct TextureBuildResult final
	{
		std::uintptr_t TextureID{};
	};

	using TextureBuilder = bool (*)(const unsigned char*, int, int,
		TextureBuildResult&, void*) noexcept;

	enum class LiveApplyResult : std::uint8_t
	{
		NoRequest,
		Applied,
		Failed
	};

	[[nodiscard]] bool BuildDefaultAtlas(ImGuiIO& a_io);
	[[nodiscard]] bool RequestAtlasRebuild(
		const FrameworkSettings::FontSettings&) noexcept;
	[[nodiscard]] bool HasPendingAtlasRebuild() noexcept;
	[[nodiscard]] LiveApplyResult ApplyPendingAtlas(
		ImGuiIO&, TextureBuilder, void*);

	[[nodiscard]] std::span<const FontEntry> GetFonts() noexcept;
	[[nodiscard]] std::optional<FontWeightAxis> GetWeightAxis(std::size_t);
	[[nodiscard]] ActiveFontInfo GetActiveInfo() noexcept;
	[[nodiscard]] inline float GetActiveUIScale() noexcept {
		return GetActiveInfo().Settings.UIScale; }
	[[nodiscard]] std::string_view GetLastApplyError() noexcept;
	[[nodiscard]] bool PushDefaultFont() noexcept;
	[[nodiscard]] bool PushFont(std::string_view) noexcept;
	[[nodiscard]] bool PopFont() noexcept;
}
