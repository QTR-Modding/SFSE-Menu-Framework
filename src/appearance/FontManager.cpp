#include "appearance/FontManager.h"
#include "appearance/AssetDiscovery.h"

#include <FontVariation.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/freetype/imgui_freetype.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace SFSEMenuFramework::FontManager
{
	using Appearance::Detail::DiscoverFiles;

	namespace
	{
		// Font discovery, configured-primary selection, fallback behavior, the
		// rebuild request/consume flow, and render-boundary atlas replacement
		// directly adapt SKSE Menu Framework 3 src/FontManager.cpp,
		// src/Hooks.cpp, and src/Config.cpp at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// Selected-only loading, bounded validation, candidate-atlas validation,
		// FreeType hinting, transactional atlas-content replacement, and DX12
		// texture handoff are SFSE-specific.
		constexpr wchar_t relativeFontDirectory[]{
			L"Data\\SFSE\\Plugins\\Fonts"
		};
		constexpr std::string_view preferredFallbackFont{
			"Jost-500-Medium.ttf"
		};
		constexpr std::string_view secondaryFallbackFont{
			"Jost-400-Book.ttf"
		};
		constexpr std::uintmax_t minimumFontBytes = 100;
		constexpr std::uintmax_t maximumFontBytes = 32 * 1024 * 1024;

		struct FontSource final
		{
			FrameworkSettings::FontSettings Settings{};
			std::optional<FontWeightAxis>    WeightAxis;
			std::vector<std::uint8_t>        Bytes;
			std::string                      ActiveName;
			std::string                      FallbackReason;

			[[nodiscard]] float RasterSize() const noexcept
			{
				return Settings.FontSizeMedium * Settings.UIScale;
			}
		};

		struct State final
		{
			std::vector<FontEntry> Fonts;
			std::optional<FrameworkSettings::FontSettings> PendingSettings;
			std::optional<FontSource> ActiveSource;
			std::string LastApplyError;
		};

		[[nodiscard]] State& GetState()
		{
			static auto* state = new State();
			return *state;
		}

		[[nodiscard]] bool HasSupportedExtension(std::string_view a_name) noexcept
		{
			if (a_name.size() < 4) {
				return false;
			}
			const auto extension = a_name.substr(a_name.size() - 4);
			return FrameworkSettings::EqualsIgnoreCaseAscii(extension, ".ttf") ||
			       FrameworkSettings::EqualsIgnoreCaseAscii(extension, ".otf");
		}

		[[nodiscard]] bool ConvertFontFileName(
			std::wstring_view a_name, std::string& a_result)
		{
			if (a_name.empty() ||
				a_name.size() >= FrameworkSettings::FontFileName{}.size()) {
				return false;
			}

			a_result.clear();
			a_result.reserve(a_name.size());
			for (const auto character : a_name) {
				const auto value = static_cast<std::uint32_t>(character);
				if (value < 0x20 || value > 0x7E) {
					a_result.clear();
					return false;
				}
				a_result.push_back(static_cast<char>(value));
			}
			return HasSupportedExtension(a_result);
		}

		[[nodiscard]] bool ReadFontFile(const std::filesystem::path& a_path,
			std::vector<std::uint8_t>& a_bytes);

		void RefreshFonts()
		{
			auto& state = GetState();
			DiscoverFiles(relativeFontDirectory, "font", true, state.Fonts,
				[](const std::filesystem::path& a_path, std::string& a_name) {
					return ConvertFontFileName(a_path.filename().native(), a_name);
				});
			logger::info("Discovered {} selectable font file(s)", state.Fonts.size());
		}

		[[nodiscard]] FontEntry* FindFont(std::string_view a_name) noexcept
		{
			for (auto& font : GetState().Fonts) {
				if (FrameworkSettings::EqualsIgnoreCaseAscii(font.Name, a_name)) {
					return &font;
				}
			}
			return nullptr;
		}

		[[nodiscard]] bool ReadFontFile(const std::filesystem::path& a_path,
			std::vector<std::uint8_t>& a_bytes)
		{
			std::error_code error;
			if (!std::filesystem::is_regular_file(a_path, error) || error) {
				return false;
			}

			const auto size = std::filesystem::file_size(a_path, error);
			if (error || size <= minimumFontBytes || size > maximumFontBytes ||
				size > static_cast<std::uintmax_t>(
					(std::numeric_limits<int>::max)())) {
				return false;
			}

			std::ifstream stream{ a_path, std::ios::binary };
			if (!stream) {
				return false;
			}

			a_bytes.resize(static_cast<std::size_t>(size));
			stream.read(reinterpret_cast<char*>(a_bytes.data()),
				static_cast<std::streamsize>(a_bytes.size()));
			return stream &&
			       stream.gcount() == static_cast<std::streamsize>(a_bytes.size());
		}

		void InspectWeightAxis(FontEntry& a_entry,
			std::span<const std::uint8_t> a_bytes) noexcept
		{
			if (a_entry.WeightAxisInspected) {
				return;
			}
			const auto axis = FontVariation::InspectWeightAxis(a_bytes);
			if (axis) {
				a_entry.WeightAxis = FontWeightAxis{
					.Minimum = axis->Minimum,
					.Default = axis->Default,
					.Maximum = axis->Maximum
				};
			}
			a_entry.WeightAxisInspected = true;
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

		void CopyAtlasConfiguration(
			const ImFontAtlas& a_source, ImFontAtlas& a_destination) noexcept
		{
			a_destination.Flags = a_source.Flags;
			a_destination.TexDesiredWidth = a_source.TexDesiredWidth;
			a_destination.TexGlyphPadding = a_source.TexGlyphPadding;
			a_destination.FontBuilderIO = a_source.FontBuilderIO;
			a_destination.FontBuilderFlags = a_source.FontBuilderFlags;
		}

		void SwapAtlasContents(ImFontAtlas& a_live, ImFontAtlas& a_candidate) noexcept
		{
			// This field-for-field swap is intentionally coupled to the vendored
			// Dear ImGui 1.90.8 ImFontAtlas layout. It installs the already-built
			// and already-uploaded candidate without mutating the live atlas until
			// every fallible operation has succeeded, while preserving io.Fonts'
			// stable address for framework consumers.
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

		[[nodiscard]] bool BuildAtlasFromSource(
			ImFontAtlas& a_atlas, const FontSource& a_source, ImFont*& a_font)
		{
			a_font = nullptr;
			a_atlas.Clear();

			ImFontConfig configuration{};
			configuration.PixelSnapH = false;
			configuration.FontBuilderFlags = 0;
			switch (a_source.Settings.Rendering) {
			case FrameworkSettings::FontRendering::Light:
				a_atlas.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
				break;
			case FrameworkSettings::FontRendering::Auto:
				a_atlas.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_ForceAutoHint;
				break;
			case FrameworkSettings::FontRendering::Native:
			default:
				a_atlas.FontBuilderFlags = 0;
				break;
			}
			configuration.RasterizerDensity = 1.0F;
			if (a_source.Bytes.empty()) {
				configuration.SizePixels = a_source.RasterSize();
				a_font = a_atlas.AddFontDefault(&configuration);
			} else {
				if (a_source.Bytes.size() > static_cast<std::size_t>(
						(std::numeric_limits<int>::max)())) {
					return false;
				}
				configuration.FontDataOwnedByAtlas = false;
				a_font = a_atlas.AddFontFromMemoryTTF(
					const_cast<std::uint8_t*>(a_source.Bytes.data()),
					static_cast<int>(a_source.Bytes.size()),
					a_source.RasterSize(),
					&configuration,
					a_atlas.GetGlyphRangesDefault());
			}

			const FontVariation::ScopedWeight weight{ a_source.WeightAxis ?
				std::optional<float>{ a_source.Settings.FontWeight } : std::nullopt };
			if (!a_font || !a_atlas.Build() || !HasPrintableAscii(*a_font)) {
				a_font = nullptr;
				a_atlas.Clear();
				return false;
			}
			return true;
		}

		[[nodiscard]] bool TryFileSource(ImFontAtlas& a_atlas, FontEntry& a_entry,
			const FrameworkSettings::FontSettings& a_settings,
			FontSource& a_source, ImFont*& a_font)
		{
			a_source.Bytes.clear();
			if (!ReadFontFile(a_entry.Path, a_source.Bytes)) {
				logger::warn("Could not read font '{}'", a_entry.Name);
				return false;
			}
			InspectWeightAxis(a_entry, a_source.Bytes);
			a_source.Settings = a_settings;
			a_source.WeightAxis = a_entry.WeightAxis;
			if (a_source.WeightAxis) {
				a_source.Settings.FontWeight = std::clamp(
					a_source.Settings.FontWeight,
					a_source.WeightAxis->Minimum,
					a_source.WeightAxis->Maximum);
			}

			if (!BuildAtlasFromSource(a_atlas, a_source, a_font)) {
				logger::warn(
					"Font '{}' could not build a complete printable-ASCII atlas",
					a_entry.Name);
				a_source.Bytes.clear();
				return false;
			}
			a_source.ActiveName = a_entry.Name;
			return true;
		}

		[[nodiscard]] bool ResolveAndBuild(ImFontAtlas& a_atlas,
			const FrameworkSettings::FontSettings& a_settings,
			FontSource& a_source, ImFont*& a_font)
		{
			a_source = FontSource{};
			const std::string_view configuredName{ a_settings.PrimaryFont.data() };
			const auto tryName = [&](std::string_view a_name) {
				auto* font = FindFont(a_name);
				return font && TryFileSource(
					a_atlas, *font, a_settings, a_source, a_font);
			};

			const bool configuredFound = FindFont(configuredName) != nullptr;
			if (configuredFound && tryName(configuredName)) {
				return true;
			}
			a_source.FallbackReason = configuredFound ?
				"Configured font could not be built; using a fallback." :
				"Configured font was not found; using a fallback.";

			for (const auto fallbackName :
				{ preferredFallbackFont, secondaryFallbackFont }) {
				if (FrameworkSettings::EqualsIgnoreCaseAscii(
						configuredName, fallbackName)) {
					continue;
				}
				if (tryName(fallbackName)) {
					logger::warn(
						"{} Active fallback: '{}'",
						a_source.FallbackReason,
						a_source.ActiveName);
					return true;
				}
			}

			a_source.Settings = a_settings;
			a_source.WeightAxis.reset();
			a_source.Bytes.clear();
			a_source.ActiveName = "ImGui embedded fallback";
			if (!BuildAtlasFromSource(a_atlas, a_source, a_font)) {
				logger::critical("Even ImGui's embedded font atlas could not be built");
				return false;
			}
			logger::warn(
				"{} Active fallback: ImGui embedded font.",
				a_source.FallbackReason);
			return true;
		}

		void CommitActive(FontSource&& a_source)
		{
			auto& state = GetState();
			state.ActiveSource = std::move(a_source);
			const auto& active = *state.ActiveSource;
			if (active.WeightAxis) {
				logger::info(
					"Loaded ImGui font '{}' at weight {:.0f}, {:.1f} logical px, "
					"{:.0f}% UI scale, {:.1f} raster px (FreeType {} hinting)",
					active.ActiveName,
					active.Settings.FontWeight,
					active.Settings.FontSizeMedium,
					active.Settings.UIScale * 100.0F,
					active.RasterSize(),
					FrameworkSettings::GetFontRenderingName(active.Settings.Rendering));
			} else {
				logger::info(
					"Loaded ImGui font '{}' at {:.1f} logical px, {:.0f}% UI scale, "
					"{:.1f} raster px (FreeType {} hinting)",
					active.ActiveName,
					active.Settings.FontSizeMedium,
					active.Settings.UIScale * 100.0F,
					active.RasterSize(),
					FrameworkSettings::GetFontRenderingName(active.Settings.Rendering));
			}
		}

		[[nodiscard]] LiveApplyResult ApplyFailed(std::string_view a_message)
		{
			auto& error = GetState().LastApplyError;
			error = a_message;
			logger::error("{}", error);
			return LiveApplyResult::Failed;
		}

		[[nodiscard]] bool GetAtlasPixels(ImFontAtlas& a_atlas,
			unsigned char*& a_pixels, int& a_width, int& a_height) noexcept
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
		RefreshFonts();
		FontSource source;
		ImFont* font{};
		if (!ResolveAndBuild(*a_io.Fonts, FrameworkSettings::GetFontSettings(),
				source, font)) {
			return false;
		}

		a_io.FontDefault = font;
		a_io.FontGlobalScale = 1.0F;
		CommitActive(std::move(source));
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
		if (state.ActiveSource && FrameworkSettings::FontSettingsEqual(
				a_settings, state.ActiveSource->Settings)) {
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

		FontSource candidateSource;
		ImFont* candidateFont{};
		if (!ResolveAndBuild(candidateAtlas, requested, candidateSource,
				candidateFont)) {
			return ApplyFailed("Could not build the requested font; the previous "
				"font remains active.");
		}

		unsigned char* candidatePixels{};
		int candidateWidth{};
		int candidateHeight{};
		if (!GetAtlasPixels(candidateAtlas, candidatePixels, candidateWidth,
				candidateHeight)) {
			return ApplyFailed("Could not read the requested font atlas; the "
				"previous font remains active.");
		}

		TextureBuildResult textureResult{};
		if (!a_textureBuilder || !a_textureBuilder(candidatePixels, candidateWidth,
				candidateHeight, textureResult, a_userData) ||
			textureResult.TextureID == 0) {
			return ApplyFailed("Could not upload the requested font; the previous "
				"font remains active.");
		}

		candidateAtlas.SetTexID(reinterpret_cast<ImTextureID>(textureResult.TextureID));
		SwapAtlasContents(*a_io.Fonts, candidateAtlas);
		a_io.FontDefault = candidateFont;
		a_io.FontGlobalScale = 1.0F;
		ImGui::SetCurrentFont(candidateFont);

		// candidateAtlas now owns the previous generation. Destroy it while the
		// previous external font bytes are still retained by ActiveSource.
		candidateAtlas.Clear();
		CommitActive(std::move(candidateSource));
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
		auto& font = fonts[a_fontIndex];
		if (!font.WeightAxisInspected) {
			std::vector<std::uint8_t> bytes;
			if (!ReadFontFile(font.Path, bytes)) {
				return std::nullopt;
			}
			InspectWeightAxis(font, bytes);
		}
		return font.WeightAxis;
	}

	ActiveFontInfo GetActiveInfo() noexcept
	{
		const auto& active = GetState().ActiveSource;
		const auto* source = active ? &*active : nullptr;
		if (!source) {
			return { .Settings = FrameworkSettings::GetFontSettings() };
		}
		return { .Settings = source->Settings, .Name = source->ActiveName,
			.WeightAxis = source->WeightAxis,
			.FallbackReason = source->FallbackReason,
			.RasterSize = source->RasterSize() };
	}

	std::string_view GetLastApplyError() noexcept
	{
		return GetState().LastApplyError;
	}
}
