#include "appearance/fonts/FontComposer.h"

#include "appearance/fonts/FontAtlasBuilder.h"
#include "appearance/fonts/GlyphRanges.h"

#include <FontVariation.h>

#include <d3d12.h>

#include <algorithm>
#include <limits>

namespace SFSEMenuFramework::Fonts
{
	// Case-insensitive filename/stem aliases, default icon merging, named icon
	// composites, and Jost text fallback adapt SKSE Menu Framework 3
	// src/FontManager.cpp at commit
	// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
	// Exact retained-byte variable-weight registration and D3D12 validation are
	// Starfield-specific.
	namespace
	{
		[[nodiscard]] std::string_view Stem(
			std::string_view a_name) noexcept
		{
			const auto dot = a_name.find_last_of('.');
			return dot == std::string_view::npos ? a_name : a_name.substr(0, dot);
		}

		void RegisterAlias(
			AtlasGeneration& a_generation,
			std::string_view a_name,
			ImFont* a_font)
		{
			if (!a_font || a_name.empty()) {
				return;
			}
			const auto registerOne = [&](std::string_view a_alias) {
				for (auto& existing : a_generation.Aliases) {
					if (FrameworkSettings::EqualsIgnoreCaseAscii(
							existing.Name, a_alias)) {
						existing.Font = a_font;
						return;
					}
				}
				a_generation.Aliases.push_back(
					{ std::string{ a_alias }, a_font });
			};
			registerOne(a_name);
			registerOne(Stem(a_name));
		}

		[[nodiscard]] ImFontConfig TextConfiguration() noexcept
		{
			ImFontConfig result{};
			// In ImGui 1.90.8, false makes AddFont copy the caller's bytes into
			// atlas-owned ConfigData. FontBuildPlan may therefore remain build-local.
			result.FontDataOwnedByAtlas = false;
			result.PixelSnapH = false;
			result.RasterizerDensity = 1.0F;
			return result;
		}

		[[nodiscard]] ImFont* AddTextFace(
			ImFontAtlas& a_atlas,
			const FontAsset& a_asset,
			float a_size,
			const ImWchar* a_ranges,
			float a_weight,
			std::vector<FontVariation::WeightRequest>& a_weights,
			bool a_merge = false)
		{
			if (a_asset.Bytes.empty() ||
				a_asset.Bytes.size() > static_cast<std::size_t>(
					(std::numeric_limits<int>::max)())) {
				return nullptr;
			}
			auto configuration = TextConfiguration();
			configuration.MergeMode = a_merge;
			auto* font = a_atlas.AddFontFromMemoryTTF(
				const_cast<std::uint8_t*>(a_asset.Bytes.data()),
				static_cast<int>(a_asset.Bytes.size()),
				a_size,
				&configuration,
				a_ranges);
			if (font && a_asset.WeightAxis && !a_atlas.ConfigData.empty()) {
				const auto& retained = a_atlas.ConfigData.back();
				if (retained.FontData && retained.FontDataSize > 0) {
					a_weights.push_back({
						.FontBytes = std::span{
							reinterpret_cast<const std::uint8_t*>(retained.FontData),
							static_cast<std::size_t>(retained.FontDataSize) },
						.Weight = std::clamp(a_weight,
							a_asset.WeightAxis->Minimum,
							a_asset.WeightAxis->Maximum)
					});
				}
			}
			return font;
		}

		[[nodiscard]] ImFont* AddEmbeddedFace(
			ImFontAtlas& a_atlas, float a_size)
		{
			auto configuration = TextConfiguration();
			configuration.FontDataOwnedByAtlas = true;
			configuration.SizePixels = a_size;
			return a_atlas.AddFontDefault(&configuration);
		}

		[[nodiscard]] bool MergeIcon(
			ImFontAtlas& a_atlas, const FontAsset& a_asset, float a_size)
		{
			if (a_asset.Bytes.empty() ||
				a_asset.Bytes.size() > static_cast<std::size_t>(
					(std::numeric_limits<int>::max)())) {
				return false;
			}
			ImFontConfig configuration{};
			configuration.FontDataOwnedByAtlas = false;
			configuration.MergeMode = true;
			configuration.PixelSnapH = true;
			configuration.RasterizerDensity = 1.0F;
			return a_atlas.AddFontFromMemoryTTF(
				const_cast<std::uint8_t*>(a_asset.Bytes.data()),
				static_cast<int>(a_asset.Bytes.size()),
				a_size,
				&configuration,
				GetIconGlyphRanges()) != nullptr;
		}

		[[nodiscard]] ImFont* AddPrimaryBase(
			ImFontAtlas& a_atlas,
			AtlasGeneration& a_generation,
			const FontBuildPlan& a_plan,
			float a_size,
			const ImWchar* a_ranges,
			std::vector<FontVariation::WeightRequest>& a_weights)
		{
			return a_plan.PrimaryAsset ?
				AddTextFace(a_atlas,
					a_plan.Assets[*a_plan.PrimaryAsset],
					a_size, a_ranges,
					a_generation.Settings.FontWeight, a_weights) :
				AddEmbeddedFace(a_atlas, a_size);
		}

		[[nodiscard]] bool AddFaces(
			ImFontAtlas& a_atlas,
			AtlasGeneration& a_generation,
			const FontBuildPlan& a_plan,
			std::vector<FontVariation::WeightRequest>& a_weights)
		{
			a_generation.DefaultFont = AddPrimaryBase(
				a_atlas, a_generation, a_plan,
				a_generation.RasterSize(),
				a_generation.TextGlyphRanges.data(), a_weights);
			if (!a_generation.DefaultFont) {
				return false;
			}
			RegisterAlias(a_generation, a_generation.ActiveName,
				a_generation.DefaultFont);

			// SKSE Menu Framework merges SkyrimMenuFont behind the primary to
			// fill missing text glyphs. This port uses the redistributable Jost
			// Book face; the primary remains first and therefore keeps precedence.
			if (a_plan.TextFallbackAsset) {
				const auto& fallback = a_plan.Assets[*a_plan.TextFallbackAsset];
				if (!AddTextFace(a_atlas, fallback,
						AssetRasterSize(fallback, a_generation.Settings),
						a_atlas.GetGlyphRangesDefault(),
						a_generation.Settings.FontWeight, a_weights, true)) {
					return false;
				}
			}

			// SKSE Menu Framework merges all three styles into the default face.
			// ImGui keeps the first glyph for overlaps, so Solid wins, then
			// Regular, then Brands, matching the pinned framework's order.
			for (const auto assetIndex : a_plan.IconAssets) {
				if (assetIndex && !MergeIcon(a_atlas,
						a_plan.Assets[*assetIndex],
						AssetRasterSize(a_plan.Assets[*assetIndex],
							a_generation.Settings))) {
					return false;
				}
			}

			for (const auto assetIndex : a_plan.NamedAssets) {
				const auto& asset = a_plan.Assets[assetIndex];
				auto* font = AddTextFace(a_atlas, asset,
					AssetRasterSize(asset, a_generation.Settings),
					a_generation.TextGlyphRanges.data(),
					a_generation.Settings.FontWeight, a_weights);
				if (!font) {
					return false;
				}
				RegisterAlias(a_generation, asset.Name, font);
			}

			for (std::size_t index = 0; index < a_plan.IconAssets.size(); ++index) {
				const auto assetIndex = a_plan.IconAssets[index];
				if (!assetIndex) {
					continue;
				}
				const auto compositeSize = AssetRasterSize(
					a_plan.Assets[*assetIndex], a_generation.Settings);
				auto* composite = AddPrimaryBase(
					a_atlas, a_generation, a_plan,
					compositeSize,
					a_generation.TextGlyphRanges.data(), a_weights);
				if (!composite || !MergeIcon(a_atlas,
						a_plan.Assets[*assetIndex], compositeSize)) {
					return false;
				}
				RegisterAlias(a_generation,
					GetIconFileName(static_cast<IconStyle>(index)), composite);
			}
			return true;
		}

		[[nodiscard]] bool HasPrintableAscii(const ImFont& a_font) noexcept
		{
			for (ImWchar character = 0x20; character <= 0x7E; ++character) {
				if (!a_font.FindGlyphNoFallback(character)) {
					return false;
				}
			}
			return true;
		}
	}

	bool ComposeAndBuildAtlas(
		ImFontAtlas& a_atlas,
		const FontBuildPlan& a_plan,
		AtlasGeneration& a_generation)
	{
		std::vector<FontVariation::WeightRequest> weights;
		weights.reserve(a_plan.Assets.size() + a_plan.IconAssets.size());
		if (!AddFaces(a_atlas, a_generation, a_plan, weights)) {
			return false;
		}
		const FontVariation::ScopedWeightTable requestedWeights{ weights };
		if (!a_atlas.Build() || !a_generation.DefaultFont ||
			!HasPrintableAscii(*a_generation.DefaultFont)) {
			return false;
		}
		if (a_atlas.TexWidth <= 0 || a_atlas.TexHeight <= 0 ||
			a_atlas.TexWidth > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
			a_atlas.TexHeight > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
			logger::warn("Rejected ImGui font atlas {}x{}; Direct3D 12 supports "
				"at most {} pixels per dimension",
				a_atlas.TexWidth, a_atlas.TexHeight,
				D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
			return false;
		}
		return true;
	}
}
