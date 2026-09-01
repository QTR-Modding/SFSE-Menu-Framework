#pragma once

#include "Config.h"

#include <cstddef>
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
		bool                          WeightAxisInspected{};
	};

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
}

namespace SFSEMenuFramework::ThemeManager
{
	struct ThemeEntry final
	{
		std::string           Name;
		std::filesystem::path Path;
	};

	void Initialize();
	void ApplyPending() noexcept;

	[[nodiscard]] std::span<const ThemeEntry> GetThemes() noexcept;
	[[nodiscard]] std::size_t GetSelectedThemeIndex() noexcept;
	[[nodiscard]] bool QueueTheme(std::size_t);
	[[nodiscard]] bool QueueConfiguredTheme();
	[[nodiscard]] bool QueueUIScale(float) noexcept;
}
