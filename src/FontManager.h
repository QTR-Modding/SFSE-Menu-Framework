#pragma once

#include "FrameworkSettings.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct ImGuiIO;

namespace SFSEMenuFramework::FontManager
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
	};

	struct TextureBuildResult final
	{
		std::uintptr_t TextureID{};
	};

	using TextureBuilder = bool (*)(
		const unsigned char*,
		int,
		int,
		TextureBuildResult&,
		void*) noexcept;

	enum class LiveApplyResult : std::uint8_t
	{
		NoRequest,
		Applied,
		Failed
	};

	[[nodiscard]] bool BuildDefaultAtlas(ImGuiIO& a_io);
	[[nodiscard]] bool RequestAtlasRebuild(
		const FrameworkSettings::FontSettings& a_settings) noexcept;
	[[nodiscard]] bool HasPendingAtlasRebuild() noexcept;
	[[nodiscard]] LiveApplyResult ApplyPendingAtlas(
		ImGuiIO&       a_io,
		TextureBuilder a_textureBuilder,
		void*          a_userData);

	[[nodiscard]] std::span<const FontEntry> GetFonts() noexcept;
	[[nodiscard]] FrameworkSettings::FontSettings GetActiveSettings() noexcept;
	[[nodiscard]] std::string_view GetActiveFontName() noexcept;
	[[nodiscard]] float            GetActiveFontSize() noexcept;
	[[nodiscard]] float            GetActiveUIScale() noexcept;
	[[nodiscard]] float            GetActiveRasterSize() noexcept;
	[[nodiscard]] float            GetActiveFontWeight() noexcept;
	[[nodiscard]] std::optional<FontWeightAxis> GetActiveWeightAxis() noexcept;
	[[nodiscard]] std::string_view GetFallbackReason() noexcept;
	[[nodiscard]] std::string_view GetLastApplyError() noexcept;
}
