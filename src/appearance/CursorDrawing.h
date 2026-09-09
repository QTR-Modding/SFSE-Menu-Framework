#pragma once

#include "appearance/ThemeImage.h"

#include <imgui.h>
#include <nlohmann/json_fwd.hpp>

namespace SFSEMenuFramework::CursorDrawing
{
	struct Style final
	{
		std::shared_ptr<const ThemeImage> Image;
		ImVec2 Size{ 32.0F, 32.0F };
		ImVec2 Hotspot{ 0.0F, 0.0F };
		bool LoadFailed{};
	};

	[[nodiscard]] Style Load(const nlohmann::json& a_definition,
		const std::filesystem::path& a_directory);
	// Returns true only when a custom pointer was drawn, so the caller can
	// suppress ImGui's arrow for this render without changing input ownership.
	[[nodiscard]] bool Draw(const Style& a_style, ImTextureID a_texture);
}
