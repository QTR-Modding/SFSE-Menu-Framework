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

	class ScopedWeight final
	{
	public:
		explicit ScopedWeight(std::optional<float> a_weight) noexcept;
		~ScopedWeight();

		ScopedWeight(const ScopedWeight&) = delete;
		ScopedWeight(ScopedWeight&&) = delete;
		ScopedWeight& operator=(const ScopedWeight&) = delete;
		ScopedWeight& operator=(ScopedWeight&&) = delete;

	private:
		std::optional<float> previous_;
	};

	[[nodiscard]] bool ApplyRequestedWeight(
		FT_Library a_library,
		FT_Face    a_face) noexcept;
	[[nodiscard]] std::optional<WeightAxis> InspectWeightAxis(
		std::span<const std::uint8_t> a_fontBytes) noexcept;
}
