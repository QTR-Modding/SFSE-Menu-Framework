#pragma once

#include "config/FrameworkSettings.h"

#include <imgui.h>

#include <vector>

namespace SFSEMenuFramework::Fonts
{
	void BuildTextGlyphRanges(
		ImFontAtlas&,
		const FrameworkSettings::GlyphCoverage&,
		std::vector<ImWchar>&);

	[[nodiscard]] const ImWchar* GetIconGlyphRanges() noexcept;
}
