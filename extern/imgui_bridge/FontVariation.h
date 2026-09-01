#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <optional>
#include <span>

namespace SFSEMenuFramework::FontVariation
{
	struct WeightAxis final
	{
		float Minimum{};
		float Default{};
		float Maximum{};
	};

	enum class FontKind : std::uint8_t
	{
		Invalid,
		Fixed,
		VariableWeight
	};

	struct FontInspection final
	{
		FontKind Kind{ FontKind::Invalid };
		bool HasUnicodeCharmap{};
		bool HasPrintableAscii{};
		std::optional<WeightAxis> Weight;
	};

	struct WeightRequest final
	{
		// ImGui must receive this same byte span as ImFontConfig::FontData.
		std::span<const std::uint8_t> FontBytes;
		float Weight{};
	};

	// Keeps a non-owning request table active for one synchronous atlas build.
	class ScopedWeightTable final
	{
	public:
		explicit ScopedWeightTable(
			std::span<const WeightRequest> a_requests) noexcept;
		~ScopedWeightTable() noexcept;

		ScopedWeightTable(const ScopedWeightTable&) = delete;
		ScopedWeightTable(ScopedWeightTable&&) = delete;
		ScopedWeightTable& operator=(const ScopedWeightTable&) = delete;
		ScopedWeightTable& operator=(ScopedWeightTable&&) = delete;

	private:
		std::span<const WeightRequest> previous_;
	};

	[[nodiscard]] bool ApplyRequestedWeight(
		FT_Library a_library,
		FT_Face    a_face,
		std::span<const std::uint8_t> a_fontBytes) noexcept;
	[[nodiscard]] FontInspection InspectFont(
		std::span<const std::uint8_t> a_fontBytes) noexcept;
}
