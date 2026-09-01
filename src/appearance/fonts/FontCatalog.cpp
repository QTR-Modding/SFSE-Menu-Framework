#include "appearance/fonts/FontCatalog.h"

#include "appearance/AssetDiscovery.h"
#include "config/FrameworkSettings.h"

#include <FontVariation.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <system_error>

namespace SFSEMenuFramework::Fonts
{
	// Sorted TTF/OTF discovery, filename normalization, and cached weight
	// inspection and adjacent fontSize JSON sidecars adapt SKSE Menu Framework
	// 3 src/FontManager.cpp at commit
	// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
	// Bounded reads, nonthrowing JSON parsing, FreeType preflight, and reserved
	// icon separation are Starfield-specific hardening.
	namespace
	{
		constexpr wchar_t relativeFontDirectory[]{
			L"Data\\SFSE\\Plugins\\Fonts"
		};
		constexpr std::array<std::string_view, 3> iconFileNames{
			"fa-solid-900.ttf",
			"fa-regular-400.ttf",
			"fa-brands-400.ttf"
		};
		constexpr std::uintmax_t minimumFontBytes = 100;
		constexpr std::uintmax_t maximumFontBytes = 32 * 1024 * 1024;
		constexpr std::uintmax_t maximumMetadataBytes = 64 * 1024;

		[[nodiscard]] bool HasSupportedExtension(
			std::string_view a_name) noexcept
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
			return HasSupportedExtension(a_result) &&
			       !IsIconFontName(a_result);
		}

		[[nodiscard]] bool ReadFontFile(
			const std::filesystem::path& a_path,
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

		[[nodiscard]] bool InspectBytes(
			FontAsset& a_asset) noexcept
		{
			const auto inspected = FontVariation::InspectFont(a_asset.Bytes);
			if (inspected.Kind == FontVariation::FontKind::Invalid ||
				!inspected.HasUnicodeCharmap) {
				return false;
			}
			a_asset.HasPrintableAscii = inspected.HasPrintableAscii;
			if (inspected.Weight) {
				a_asset.WeightAxis = FontWeightAxis{
					.Minimum = inspected.Weight->Minimum,
					.Default = inspected.Weight->Default,
					.Maximum = inspected.Weight->Maximum
				};
			}
			return true;
		}

		[[nodiscard]] std::optional<float> ReadSizeOverride(
			const std::filesystem::path& a_fontPath)
		{
			auto metadataPath = a_fontPath;
			metadataPath.replace_extension(L".json");

			std::error_code error;
			const bool exists = std::filesystem::exists(metadataPath, error);
			if (error) {
				logger::warn("Could not inspect font metadata '{}': {}",
					metadataPath.string(), error.message());
				return std::nullopt;
			}
			if (!exists) {
				return std::nullopt;
			}

			const auto size = std::filesystem::file_size(metadataPath, error);
			if (error) {
				logger::warn("Could not read font metadata '{}': {}",
					metadataPath.string(), error.message());
				return std::nullopt;
			}
			if (size > maximumMetadataBytes) {
				logger::warn("Font metadata '{}' exceeds the {} byte safety limit; "
					"ignoring it", metadataPath.string(), maximumMetadataBytes);
				return std::nullopt;
			}

			std::ifstream stream{ metadataPath, std::ios::binary };
			if (!stream) {
				logger::warn("Could not open font metadata '{}'", metadataPath.string());
				return std::nullopt;
			}
			std::string contents(static_cast<std::size_t>(size), '\0');
			stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
			if (stream.gcount() != static_cast<std::streamsize>(contents.size())) {
				logger::warn("Could not read complete font metadata '{}'",
					metadataPath.string());
				return std::nullopt;
			}
			if (stream.peek() != std::char_traits<char>::eof()) {
				logger::warn("Font metadata '{}' changed or exceeded its bounded size "
					"while being read; ignoring it", metadataPath.string());
				return std::nullopt;
			}
			const auto document =
				nlohmann::json::parse(contents, nullptr, false, false);
			if (document.is_discarded()) {
				logger::warn("Font metadata '{}' is not valid JSON",
					metadataPath.string());
				return std::nullopt;
			}
			if (!document.is_object()) {
				return std::nullopt;
			}

			const auto value = document.find("fontSize");
			if (value == document.end() || !value->is_number()) {
				return std::nullopt;
			}

			double configured{};
			if (const auto* floatValue = value->get_ptr<
					const nlohmann::json::number_float_t*>()) {
				configured = *floatValue;
			} else if (const auto* integerValue = value->get_ptr<
					const nlohmann::json::number_integer_t*>()) {
				configured = static_cast<double>(*integerValue);
			} else if (const auto* unsignedValue = value->get_ptr<
					const nlohmann::json::number_unsigned_t*>()) {
				configured = static_cast<double>(*unsignedValue);
			}
			if (configured <= 0.0) {
				return std::nullopt;
			}
			if (!std::isfinite(configured) ||
				configured > static_cast<double>(
					(std::numeric_limits<float>::max)()) ||
				static_cast<float>(configured) <= 0.0F) {
				logger::warn("Font metadata '{}' has an unsafe fontSize; ignoring it",
					metadataPath.string());
				return std::nullopt;
			}
			return static_cast<float>(configured);
		}
	}

	void Refresh(std::vector<FontEntry>& a_fonts)
	{
		Appearance::Detail::DiscoverFiles(
			relativeFontDirectory, "font", true, a_fonts,
			[](const std::filesystem::path& a_path, std::string& a_name) {
				return ConvertFontFileName(a_path.filename().native(), a_name);
			});
	}

	FontEntry* FindExact(
		std::span<FontEntry> a_fonts, std::string_view a_name) noexcept
	{
		for (auto& font : a_fonts) {
			if (FrameworkSettings::EqualsIgnoreCaseAscii(font.Name, a_name)) {
				return &font;
			}
		}
		return nullptr;
	}

	bool Load(FontEntry& a_entry, FontAsset& a_asset)
	{
		a_asset = FontAsset{};
		a_asset.Name = a_entry.Name;
		if (!ReadFontFile(a_entry.Path, a_asset.Bytes) ||
			!InspectBytes(a_asset)) {
			a_asset = FontAsset{};
			return false;
		}
		a_asset.SizeOverride = ReadSizeOverride(a_entry.Path);
		a_entry.WeightAxis = a_asset.WeightAxis;
		a_entry.WeightAxisInspected = true;
		return true;
	}

	bool LoadIconAsset(IconStyle a_style, FontAsset& a_asset)
	{
		a_asset = FontAsset{};
		a_asset.Name = GetIconFileName(a_style);
		auto path = FrameworkSettings::BuildGamePath(relativeFontDirectory);
		if (path.empty()) {
			return false;
		}
		path /= a_asset.Name;
		if (!ReadFontFile(path, a_asset.Bytes) || !InspectBytes(a_asset)) {
			a_asset = FontAsset{};
			return false;
		}
		a_asset.SizeOverride = ReadSizeOverride(path);
		return true;
	}

	bool Inspect(FontEntry& a_entry)
	{
		if (a_entry.WeightAxisInspected) {
			return true;
		}
		FontAsset asset;
		if (!Load(a_entry, asset)) {
			a_entry.WeightAxis.reset();
			a_entry.WeightAxisInspected = true;
			return false;
		}
		return true;
	}

	std::string_view GetIconFileName(IconStyle a_style) noexcept
	{
		const auto index = static_cast<std::size_t>(a_style);
		return index < iconFileNames.size() ? iconFileNames[index] :
			iconFileNames.front();
	}

	bool IsIconFontName(std::string_view a_name) noexcept
	{
		for (const auto iconName : iconFileNames) {
			if (FrameworkSettings::EqualsIgnoreCaseAscii(a_name, iconName)) {
				return true;
			}
		}
		return false;
	}
}
