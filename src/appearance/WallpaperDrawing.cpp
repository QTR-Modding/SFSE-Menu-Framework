#include "appearance/WallpaperDrawing.h"

#include <imgui_internal.h>

namespace SFSEMenuFramework::WallpaperDrawing
{
	void RenderCurrentWindow(ImTextureID a_texture, ImVec2 a_imageSize,
		float a_opacity, float a_brightness) noexcept
	{
		if (!a_texture || a_opacity <= 0.0F || !ImGui::GetCurrentContext() ||
			a_imageSize.x <= 0.0F || a_imageSize.y <= 0.0F) {
			return;
		}
		const auto* window = ImGui::GetCurrentWindow();
		if (window->SkipItems || (window->Flags & ImGuiWindowFlags_ChildWindow)) {
			return;
		}
		const auto area = window->InnerRect;
		const auto size = area.GetSize();
		if (size.x <= 0.0F || size.y <= 0.0F) {
			return;
		}

		ImVec2 uvMinimum{ 0.0F, 0.0F };
		ImVec2 uvMaximum{ 1.0F, 1.0F };
		const auto imageAspect = a_imageSize.x / a_imageSize.y;
		const auto windowAspect = size.x / size.y;
		if (imageAspect > windowAspect) {
			const auto visible = windowAspect / imageAspect;
			uvMinimum.x = (1.0F - visible) * 0.5F;
			uvMaximum.x = uvMinimum.x + visible;
		} else {
			const auto visible = imageAspect / windowAspect;
			uvMinimum.y = (1.0F - visible) * 0.5F;
			uvMaximum.y = uvMinimum.y + visible;
		}

		auto* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(area.Min, area.Max, true);
		drawList->AddImage(a_texture, area.Min, area.Max, uvMinimum, uvMaximum,
			ImGui::GetColorU32(ImVec4{
				a_brightness, a_brightness, a_brightness, a_opacity }));
		drawList->PopClipRect();
	}
}
