#pragma once

#include <imgui.h>

namespace SFSEMenuFramework::WallpaperDrawing
{
	// Fills the root window without stretching; child panels reveal this image.
	void RenderCurrentWindow(ImTextureID, ImVec2 a_imageSize,
		float a_opacity, float a_brightness) noexcept;
}
