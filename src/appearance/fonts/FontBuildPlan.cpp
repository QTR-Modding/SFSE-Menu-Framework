#include "appearance/fonts/FontBuildPlan.h"

#include "config/FrameworkSettingsInternal.h"

#include <cmath>

namespace SFSEMenuFramework::Fonts
{
	// Primary/fallback selection, named font discovery, and per-face sidecar
	// sizing adapt SKSE Menu Framework 3 src/FontManager.cpp at commit
	// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
	namespace
	{
		void ValidateSizeOverride(
			FontAsset& a_asset,
			const FrameworkSettings::FontSettings& a_settings) noexcept
		{
			if (!a_asset.SizeOverride) {
				return;
			}
			const auto rasterSize = static_cast<double>(*a_asset.SizeOverride) *
				static_cast<double>(a_settings.UIScale);
			if (std::isfinite(rasterSize) && rasterSize > 0.0 &&
				rasterSize <= FrameworkSettings::Detail::maximumRasterSize) {
				return;
			}
			logger::warn(
				"Font '{}' requests {:.3f} logical px at {:.0f}% UI scale, "
				"which exceeds the safe {:.0f} raster-pixel limit; ignoring "
				"its fontSize override",
				a_asset.Name, *a_asset.SizeOverride, a_settings.UIScale * 100.0F,
				FrameworkSettings::Detail::maximumRasterSize);
			a_asset.SizeOverride.reset();
		}

		[[nodiscard]] std::optional<std::size_t> FindAsset(
			const FontBuildPlan& a_plan, std::string_view a_name) noexcept
		{
			for (std::size_t index = 0; index < a_plan.Assets.size(); ++index) {
				if (FrameworkSettings::EqualsIgnoreCaseAscii(
						a_plan.Assets[index].Name, a_name)) {
					return index;
				}
			}
			return std::nullopt;
		}

		[[nodiscard]] bool SelectPrimary(
			std::span<FontEntry> a_fonts,
			const FrameworkSettings::FontSettings& a_settings,
			const FontBuildOptions& a_options,
			FontBuildPlan& a_plan)
		{
			const std::string_view configuredName{ a_settings.PrimaryFont.data() };
			const auto tryName = [&](std::string_view a_name) {
				auto* entry = FindExact(a_fonts, a_name);
				FontAsset asset;
				if (!entry || !Load(*entry, asset)) {
					return false;
				}
				ValidateSizeOverride(asset, a_settings);
				a_plan.PrimaryAsset = a_plan.Assets.size();
				a_plan.PrimaryWeightAxis = asset.WeightAxis;
				a_plan.ActiveName = asset.Name;
				a_plan.Assets.push_back(std::move(asset));
				return true;
			};

			if (a_options.Primary == PrimaryMode::Embedded) {
				a_plan.ActiveName = "ImGui embedded fallback";
				return true;
			}
			if (a_options.Primary == PrimaryMode::Exact) {
				return tryName(a_options.ExactPrimary);
			}

			const bool configuredFound = FindExact(a_fonts, configuredName) != nullptr;
			if (configuredFound && tryName(configuredName)) {
				return true;
			}
			a_plan.FallbackReason = configuredFound ?
				"Configured font could not be loaded; using a fallback." :
				"Configured font was not found; using a fallback.";
			for (const auto fallback :
				{ preferredFallbackFont, secondaryFallbackFont }) {
				if (!FrameworkSettings::EqualsIgnoreCaseAscii(
						configuredName, fallback) && tryName(fallback)) {
					return true;
				}
			}

			a_plan.ActiveName = "ImGui embedded fallback";
			return true;
		}

		void LoadTextFallback(
			std::span<FontEntry> a_fonts,
			const FrameworkSettings::FontSettings& a_settings,
			FontBuildPlan& a_plan)
		{
			if (!a_plan.PrimaryAsset || FrameworkSettings::EqualsIgnoreCaseAscii(
					a_plan.Assets[*a_plan.PrimaryAsset].Name,
					secondaryFallbackFont)) {
				return;
			}

			auto* entry = FindExact(a_fonts, secondaryFallbackFont);
			FontAsset asset;
			if (!entry || !Load(*entry, asset) || !asset.HasPrintableAscii) {
				logger::warn("Could not load text fallback '{}'",
					secondaryFallbackFont);
				return;
			}
			ValidateSizeOverride(asset, a_settings);
			a_plan.TextFallbackAsset = a_plan.Assets.size();
			a_plan.Assets.push_back(std::move(asset));
		}

		void LoadNamedFonts(
			std::span<FontEntry> a_fonts,
			const FrameworkSettings::FontSettings& a_settings,
			FontBuildPlan& a_plan)
		{
			const auto primaryName = a_plan.PrimaryAsset ?
				std::string_view{ a_plan.Assets[*a_plan.PrimaryAsset].Name } :
				std::string_view{};
			for (auto& entry : a_fonts) {
				if (!primaryName.empty() && FrameworkSettings::EqualsIgnoreCaseAscii(
						entry.Name, primaryName)) {
					continue;
				}
				if (const auto existing = FindAsset(a_plan, entry.Name)) {
					a_plan.NamedAssets.push_back(*existing);
					continue;
				}
				FontAsset asset;
				if (Load(entry, asset)) {
					ValidateSizeOverride(asset, a_settings);
					a_plan.NamedAssets.push_back(a_plan.Assets.size());
					a_plan.Assets.push_back(std::move(asset));
				} else {
					logger::warn("Could not load named font '{}'", entry.Name);
				}
			}
		}

		void LoadIcons(
			const FrameworkSettings::FontSettings& a_settings,
			FontBuildPlan& a_plan)
		{
			for (std::size_t index = 0; index < a_plan.IconAssets.size(); ++index) {
				FontAsset asset;
				const auto style = static_cast<IconStyle>(index);
				if (!LoadIconAsset(style, asset)) {
					logger::warn("Could not load Font Awesome face '{}'",
						GetIconFileName(style));
					continue;
				}
				ValidateSizeOverride(asset, a_settings);
				a_plan.IconAssets[index] = a_plan.Assets.size();
				a_plan.Assets.push_back(std::move(asset));
			}
		}
	}

	float ConfiguredRasterSize(
		const FrameworkSettings::FontSettings& a_settings) noexcept
	{
		return a_settings.FontSizeMedium * a_settings.UIScale;
	}

	float AssetRasterSize(
		const FontAsset& a_asset,
		const FrameworkSettings::FontSettings& a_settings) noexcept
	{
		const auto logicalSize =
			a_asset.SizeOverride.value_or(a_settings.FontSizeMedium);
		return logicalSize * a_settings.UIScale;
	}

	bool PrepareFontBuildPlan(
		std::span<FontEntry> a_fonts,
		const FrameworkSettings::FontSettings& a_settings,
		const FontBuildOptions& a_options,
		FontBuildPlan& a_plan)
	{
		a_plan.Assets.reserve(a_fonts.size() + 4);
		a_plan.NamedAssets.reserve(a_fonts.size());
		if (!SelectPrimary(a_fonts, a_settings, a_options, a_plan)) {
			return false;
		}

		a_plan.DefaultRasterSize = a_plan.PrimaryAsset ?
			AssetRasterSize(a_plan.Assets[*a_plan.PrimaryAsset], a_settings) :
			ConfiguredRasterSize(a_settings);
		if (!a_options.FallbackReason.empty()) {
			a_plan.FallbackReason = a_options.FallbackReason;
		}
		if (a_options.IncludeTextFallback) {
			LoadTextFallback(a_fonts, a_settings, a_plan);
		}
		if (a_options.IncludeNamedFonts) {
			LoadNamedFonts(a_fonts, a_settings, a_plan);
		}
		if (a_options.IncludeIcons) {
			LoadIcons(a_settings, a_plan);
		}
		return true;
	}
}
