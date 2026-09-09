#pragma once

#include <imgui.h>

#include <cstdint>

namespace SFSEMenuFramework::ThemeBackdrop
{
	enum class Effect : std::uint8_t
	{
		None,
		Stars,
		Wallpaper
	};

	struct Style final
	{
		Effect Kind{ Effect::None };
		ImVec4 StarColor{ 0.82F, 0.90F, 0.96F, 0.42F };
		ImVec4 AccentColor{ 0.91F, 0.76F, 0.38F, 0.55F };
		float Density{ 0.5F };
		float ImageOpacity{ 1.0F };
		float ImageDarkening{ 0.0F };
	};

	void RenderCurrentWindow(const Style&, float a_opacity) noexcept;

}
