#include "appearance/fonts/FontAtlasBuilder.h"

#include "appearance/fonts/FontBuildPlan.h"
#include "appearance/fonts/FontComposer.h"
#include "appearance/fonts/GlyphRanges.h"

#include <misc/freetype/imgui_freetype.h>

#include <algorithm>

namespace SFSEMenuFramework::Fonts
{
	namespace
	{
		[[nodiscard]] std::string_view BaseName(
			std::string_view a_name) noexcept
		{
			const auto separator = a_name.find_last_of("/\\");
			return separator == std::string_view::npos ? a_name :
				a_name.substr(separator + 1);
		}

		void ConfigureAtlas(
			ImFontAtlas& a_atlas,
			const FrameworkSettings::FontSettings& a_settings) noexcept
		{
			a_atlas.Clear();
			// Keep SDK-native atlas methods on the host's FreeType builder.
			a_atlas.FontBuilderIO = ImGuiFreeType::GetBuilderForFreeType();
			a_atlas.Flags &= ~ImFontAtlasFlags_NoPowerOfTwoHeight;
			a_atlas.TexDesiredWidth = 0;
			a_atlas.FontBuilderFlags = 0;
			switch (a_settings.Rendering) {
			case FrameworkSettings::FontRendering::Light:
				a_atlas.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
				break;
			case FrameworkSettings::FontRendering::Auto:
				a_atlas.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_ForceAutoHint;
				break;
			case FrameworkSettings::FontRendering::Native:
			default:
				break;
			}
			if (a_settings.Glyphs.ChineseSimplifiedCommon ||
				a_settings.Glyphs.ChineseFull) {
				a_atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
				a_atlas.TexDesiredWidth = 16384;
			}
		}

		[[nodiscard]] bool BuildAttempt(
			ImFontAtlas& a_atlas,
			std::span<FontEntry> a_fonts,
			const FrameworkSettings::FontSettings& a_settings,
			const FontBuildOptions& a_options,
			AtlasGeneration& a_generation,
			bool& a_usedEmbedded)
		{
			a_generation = AtlasGeneration{};
			a_generation.Settings = a_settings;
			a_generation.Aliases.reserve((a_fonts.size() + 3) * 2);
			ConfigureAtlas(a_atlas, a_settings);
			BuildTextGlyphRanges(a_atlas, a_settings.Glyphs,
				a_generation.TextGlyphRanges);

			FontBuildPlan plan;
			if (!PrepareFontBuildPlan(
					a_fonts, a_settings, a_options, plan)) {
				return false;
			}
			a_usedEmbedded = !plan.PrimaryAsset.has_value();
			a_generation.WeightAxis = plan.PrimaryWeightAxis;
			a_generation.DefaultRasterSize = plan.DefaultRasterSize;
			a_generation.ActiveName = std::move(plan.ActiveName);
			a_generation.FallbackReason = std::move(plan.FallbackReason);
			if (a_generation.WeightAxis) {
				a_generation.Settings.FontWeight = std::clamp(
					a_generation.Settings.FontWeight,
					a_generation.WeightAxis->Minimum,
					a_generation.WeightAxis->Maximum);
			}
			return ComposeAndBuildAtlas(a_atlas, plan, a_generation);
		}

		[[nodiscard]] std::string RecoveryReason(
			std::string_view a_initialReason, bool a_skipIcons)
		{
			std::string result{ a_initialReason };
			if (!result.empty()) {
				result.push_back(' ');
			}
			result += a_skipIcons ?
				"The selected font generation could not be built with icons; "
				"optional named and icon faces were skipped." :
				"The complete named font set could not be built; optional named "
				"faces were skipped.";
			return result;
		}
	}

	float AtlasGeneration::RasterSize() const noexcept
	{
		return DefaultRasterSize;
	}

	ImFont* AtlasGeneration::Find(std::string_view a_name) const noexcept
	{
		const auto name = BaseName(a_name);
		for (const auto& alias : Aliases) {
			if (FrameworkSettings::EqualsIgnoreCaseAscii(alias.Name, name)) {
				return alias.Font;
			}
		}
		return nullptr;
	}

	bool BuildAtlas(
		ImFontAtlas& a_atlas,
		std::span<FontEntry> a_fonts,
		const FrameworkSettings::FontSettings& a_settings,
		AtlasGeneration& a_generation)
	{
		bool usedEmbedded{};
		if (BuildAttempt(a_atlas, a_fonts, a_settings, FontBuildOptions{},
				a_generation, usedEmbedded)) {
			return true;
		}

		const std::string selectedPrimary{ a_generation.ActiveName };
		const auto namedRecoveryReason = RecoveryReason(
			a_generation.FallbackReason, false);
		const auto iconRecoveryReason = RecoveryReason(
			a_generation.FallbackReason, true);
		logger::warn("Complete font atlas build failed; retrying without "
			"optional named faces");

		const auto retry = [&](const FontBuildOptions& a_options) {
			bool ignoredEmbedded{};
			return BuildAttempt(a_atlas, a_fonts, a_settings, a_options,
				a_generation, ignoredEmbedded);
		};
		const auto retrySelected = [&](bool a_includeIcons,
			bool a_includeTextFallback, std::string_view a_reason) {
			return usedEmbedded ? retry({
				.Primary = PrimaryMode::Embedded,
				.IncludeNamedFonts = false,
				.IncludeIcons = a_includeIcons,
				.IncludeTextFallback = a_includeTextFallback,
				.FallbackReason = a_reason }) :
				!selectedPrimary.empty() && retry({
					.Primary = PrimaryMode::Exact,
					.ExactPrimary = selectedPrimary,
					.IncludeNamedFonts = false,
					.IncludeIcons = a_includeIcons,
					.IncludeTextFallback = a_includeTextFallback,
					.FallbackReason = a_reason });
		};
		if (retrySelected(true, true, namedRecoveryReason)) {
			return true;
		}
		if (retrySelected(false, true, iconRecoveryReason)) {
			return true;
		}
		if (!usedEmbedded && retrySelected(false, false,
				"The selected font generation could not be built with its text "
				"fallback; optional named, icon, and fallback text faces were "
				"skipped.")) {
			return true;
		}

		const std::string primaryFallbackReason{
			"The requested font generation could not be built; using a fallback "
			"without optional named faces."
		};
		for (const auto fallback :
			{ preferredFallbackFont, secondaryFallbackFont }) {
			if (!selectedPrimary.empty() &&
				FrameworkSettings::EqualsIgnoreCaseAscii(
					selectedPrimary, fallback)) {
				continue;
			}
			if (retry({
					.Primary = PrimaryMode::Exact,
					.ExactPrimary = fallback,
					.IncludeNamedFonts = false,
					.FallbackReason = primaryFallbackReason })) {
				return true;
			}
			if (retry({
					.Primary = PrimaryMode::Exact,
					.ExactPrimary = fallback,
					.IncludeNamedFonts = false,
					.IncludeIcons = false,
					.IncludeTextFallback = false,
					.FallbackReason =
						"The requested font generation and icon composites could not "
						"be built; using a fallback without optional named, icon, or "
						"fallback text faces." })) {
				return true;
			}
		}

		if (!usedEmbedded && retry({
			.Primary = PrimaryMode::Embedded,
			.IncludeNamedFonts = false,
			.FallbackReason = primaryFallbackReason })) {
			return true;
		}
		return !usedEmbedded && retry({
			.Primary = PrimaryMode::Embedded,
			.IncludeNamedFonts = false,
			.IncludeIcons = false,
			.FallbackReason =
				"Font and icon generation could not be built; using only ImGui's "
				"embedded fallback."
		});
	}
}
