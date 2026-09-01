#pragma once

#include "appearance/fonts/FontBuildPlan.h"

#include <imgui.h>

namespace SFSEMenuFramework::Fonts
{
	struct AtlasGeneration;

	[[nodiscard]] bool ComposeAndBuildAtlas(
		ImFontAtlas&,
		const FontBuildPlan&,
		AtlasGeneration&);
}
