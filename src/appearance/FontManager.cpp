#include "appearance/FontManager.h"

#include "appearance/fonts/ConsumerFontScope.h"
#include "appearance/fonts/FontAtlasBuilder.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SFSEMenuFramework::FontManager
{
	namespace
	{
		// Atlas rebuild timing and transactional replacement adapt SKSE Menu
		// Framework 3 src/FontManager.cpp and src/Hooks.cpp at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// Atlas-owned font copies, checked DX12 upload, stable io.Fonts address,
		// and safe consumer stack scoping are Starfield-specific.
		struct State final
		{
			std::vector<FontEntry> Fonts;
			std::optional<FrameworkSettings::FontSettings> PendingSettings;
			std::optional<Fonts::AtlasGeneration> ActiveGeneration;
			std::string LastApplyError;
		};

		[[nodiscard]] State& GetState()
		{
			static auto* state = new State();
			return *state;
		}

		void RefreshFonts()
		{
			auto& fonts = GetState().Fonts;
			Fonts::Refresh(fonts);
			logger::info("Discovered {} selectable font file(s)", fonts.size());
		}

		void CopyAtlasConfiguration(
			const ImFontAtlas& a_source, ImFontAtlas& a_destination) noexcept
		{
			a_destination.Flags = a_source.Flags;
			a_destination.TexDesiredWidth = a_source.TexDesiredWidth;
			a_destination.TexGlyphPadding = a_source.TexGlyphPadding;
			a_destination.FontBuilderIO = a_source.FontBuilderIO;
			a_destination.FontBuilderFlags = a_source.FontBuilderFlags;
		}

		void SwapAtlasContents(
			ImFontAtlas& a_live, ImFontAtlas& a_candidate) noexcept
		{
			// Coupled deliberately to the vendored Dear ImGui 1.90.8-docking layout.
			// The candidate is fully built and uploaded before the stable live
			// atlas object changes.
			using std::swap;
			swap(a_live.Flags, a_candidate.Flags);
			swap(a_live.TexID, a_candidate.TexID);
			swap(a_live.TexDesiredWidth, a_candidate.TexDesiredWidth);
			swap(a_live.TexGlyphPadding, a_candidate.TexGlyphPadding);
			swap(a_live.TexReady, a_candidate.TexReady);
			swap(a_live.TexPixelsUseColors, a_candidate.TexPixelsUseColors);
			swap(a_live.TexPixelsAlpha8, a_candidate.TexPixelsAlpha8);
			swap(a_live.TexPixelsRGBA32, a_candidate.TexPixelsRGBA32);
			swap(a_live.TexWidth, a_candidate.TexWidth);
			swap(a_live.TexHeight, a_candidate.TexHeight);
			swap(a_live.TexUvScale, a_candidate.TexUvScale);
			swap(a_live.TexUvWhitePixel, a_candidate.TexUvWhitePixel);
			a_live.Fonts.swap(a_candidate.Fonts);
			a_live.CustomRects.swap(a_candidate.CustomRects);
			a_live.ConfigData.swap(a_candidate.ConfigData);
			for (std::size_t index = 0; index < std::size(a_live.TexUvLines); ++index) {
				swap(a_live.TexUvLines[index], a_candidate.TexUvLines[index]);
			}
			swap(a_live.FontBuilderIO, a_candidate.FontBuilderIO);
			swap(a_live.FontBuilderFlags, a_candidate.FontBuilderFlags);
			swap(a_live.PackIdMouseCursors, a_candidate.PackIdMouseCursors);
			swap(a_live.PackIdLines, a_candidate.PackIdLines);

			for (auto* font : a_live.Fonts) {
				font->ContainerAtlas = &a_live;
			}
			for (auto* font : a_candidate.Fonts) {
				font->ContainerAtlas = &a_candidate;
			}
		}

		void CommitActive(Fonts::AtlasGeneration&& a_generation)
		{
			auto& state = GetState();
			state.ActiveGeneration = std::move(a_generation);
			const auto& active = *state.ActiveGeneration;
			if (active.WeightAxis) {
				logger::info(
					"Loaded ImGui font generation '{}' at weight {:.0f}, {:.1f} "
					"logical px, {:.0f}% UI scale, {:.1f} raster px ({} aliases)",
					active.ActiveName,
					active.Settings.FontWeight,
					active.Settings.FontSizeMedium,
					active.Settings.UIScale * 100.0F,
					active.RasterSize(),
					active.Aliases.size());
			} else {
				logger::info(
					"Loaded ImGui font generation '{}' at {:.1f} logical px, "
					"{:.0f}% UI scale, {:.1f} raster px ({} aliases)",
					active.ActiveName,
					active.Settings.FontSizeMedium,
					active.Settings.UIScale * 100.0F,
					active.RasterSize(),
					active.Aliases.size());
			}
			if (!active.FallbackReason.empty()) {
				logger::warn("{} Active fallback: '{}'",
					active.FallbackReason, active.ActiveName);
			}
		}

		[[nodiscard]] LiveApplyResult ApplyFailed(std::string_view a_message)
		{
			auto& error = GetState().LastApplyError;
			error = a_message;
			logger::error("{}", error);
			return LiveApplyResult::Failed;
		}

		[[nodiscard]] bool GetAtlasPixels(
			ImFontAtlas& a_atlas,
			unsigned char*& a_pixels,
			int& a_width,
			int& a_height) noexcept
		{
			a_pixels = nullptr;
			a_width = 0;
			a_height = 0;
			a_atlas.GetTexDataAsRGBA32(&a_pixels, &a_width, &a_height);
			return a_pixels && a_width > 0 && a_height > 0 &&
			       static_cast<std::uint64_t>(a_width) *
				       static_cast<std::uint64_t>(a_height) * 4 <=
				       (std::numeric_limits<std::size_t>::max)();
		}
	}

	bool BuildDefaultAtlas(ImGuiIO& a_io)
	{
		if (!a_io.Fonts) {
			return false;
		}
		RefreshFonts();
		Fonts::AtlasGeneration generation;
		if (!Fonts::BuildAtlas(*a_io.Fonts, GetState().Fonts,
				FrameworkSettings::GetFontSettings(), generation)) {
			a_io.Fonts->Clear();
			return false;
		}

		a_io.FontDefault = generation.DefaultFont;
		a_io.FontGlobalScale = 1.0F;
		CommitActive(std::move(generation));
		return true;
	}

	bool RequestAtlasRebuild(
		const FrameworkSettings::FontSettings& a_settings) noexcept
	{
		if (!FrameworkSettings::ValidateFontSettings(a_settings)) {
			return false;
		}
		auto& state = GetState();
		state.LastApplyError.clear();
		if (state.ActiveGeneration && FrameworkSettings::FontSettingsEqual(
				a_settings, state.ActiveGeneration->Settings)) {
			state.PendingSettings.reset();
			return true;
		}
		state.PendingSettings = a_settings;
		return true;
	}

	bool HasPendingAtlasRebuild() noexcept
	{
		return GetState().PendingSettings.has_value();
	}

	LiveApplyResult ApplyPendingAtlas(
		ImGuiIO& a_io, TextureBuilder a_textureBuilder, void* a_userData)
	{
		auto& state = GetState();
		if (!state.PendingSettings) {
			return LiveApplyResult::NoRequest;
		}
		const auto requested = *state.PendingSettings;
		state.PendingSettings.reset();

		auto* context = ImGui::GetCurrentContext();
		if (!context || &context->IO != &a_io || !a_io.Fonts ||
			context->WithinFrameScope || a_io.Fonts->Locked ||
			!context->FontStack.empty()) {
			return ApplyFailed("Could not apply the requested font at a safe "
				"ImGui frame boundary; the previous font remains active.");
		}

		ImFontAtlas candidateAtlas;
		CopyAtlasConfiguration(*a_io.Fonts, candidateAtlas);
		RefreshFonts();
		Fonts::AtlasGeneration candidateGeneration;
		if (!Fonts::BuildAtlas(candidateAtlas, state.Fonts, requested,
				candidateGeneration)) {
			candidateAtlas.Clear();
			return ApplyFailed("Could not build the requested font generation; "
				"the previous generation remains active.");
		}

		unsigned char* candidatePixels{};
		int candidateWidth{};
		int candidateHeight{};
		if (!GetAtlasPixels(candidateAtlas, candidatePixels, candidateWidth,
				candidateHeight)) {
			candidateAtlas.Clear();
			return ApplyFailed("Could not read the requested font atlas; the "
				"previous generation remains active.");
		}

		TextureBuildResult textureResult{};
		if (!a_textureBuilder || !a_textureBuilder(candidatePixels, candidateWidth,
				candidateHeight, textureResult, a_userData) ||
			textureResult.TextureID == 0) {
			candidateAtlas.Clear();
			return ApplyFailed("Could not upload the requested font atlas; the "
				"previous generation remains active.");
		}

		candidateAtlas.SetTexID(
			reinterpret_cast<ImTextureID>(textureResult.TextureID));
		SwapAtlasContents(*a_io.Fonts, candidateAtlas);
		a_io.FontDefault = candidateGeneration.DefaultFont;
		a_io.FontGlobalScale = 1.0F;
		ImGui::SetCurrentFont(candidateGeneration.DefaultFont);

		// candidateAtlas now owns the previous atlas and its retained font copies.
		candidateAtlas.Clear();
		CommitActive(std::move(candidateGeneration));
		state.LastApplyError.clear();
		return LiveApplyResult::Applied;
	}

	std::span<const FontEntry> GetFonts() noexcept
	{
		return GetState().Fonts;
	}

	std::optional<FontWeightAxis> GetWeightAxis(std::size_t a_fontIndex)
	{
		auto& fonts = GetState().Fonts;
		if (a_fontIndex >= fonts.size()) {
			return std::nullopt;
		}
		static_cast<void>(Fonts::Inspect(fonts[a_fontIndex]));
		return fonts[a_fontIndex].WeightAxis;
	}

	ActiveFontInfo GetActiveInfo() noexcept
	{
		const auto& active = GetState().ActiveGeneration;
		if (!active) {
			return { .Settings = FrameworkSettings::GetFontSettings() };
		}
		return {
			.Settings = active->Settings,
			.Name = active->ActiveName,
			.WeightAxis = active->WeightAxis,
			.FallbackReason = active->FallbackReason,
			.RasterSize = active->RasterSize()
		};
	}

	std::string_view GetLastApplyError() noexcept
	{
		return GetState().LastApplyError;
	}

	bool PushDefaultFont() noexcept
	{
		if (!ConsumerFontScope::IsActive()) {
			return false;
		}
		const auto& active = GetState().ActiveGeneration;
		return active && ConsumerFontScope::Push(active->DefaultFont);
	}

	bool PushFont(std::string_view a_name) noexcept
	{
		if (!ConsumerFontScope::IsActive()) {
			return false;
		}
		const auto& active = GetState().ActiveGeneration;
		return active && ConsumerFontScope::Push(active->Find(a_name));
	}

	bool PopFont() noexcept
	{
		return ConsumerFontScope::Pop();
	}
}
