#include "appearance/CursorDrawing.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>

namespace SFSEMenuFramework::CursorDrawing
{
	namespace
	{
		bool ReadPair(const nlohmann::json& a_object, const char* a_name,
			ImVec2& a_result, float a_minimum, float a_maximum)
		{
			const auto field = a_object.find(a_name);
			if (field == a_object.end()) {
				return true;
			}
			if (!field->is_array() || field->size() != 2) {
				return false;
			}
			float values[2]{};
			for (std::size_t i = 0; i < 2; ++i) {
				if (!(*field)[i].is_number()) {
					return false;
				}
				const auto value = (*field)[i].get<double>();
				if (!std::isfinite(value) || value < a_minimum || value > a_maximum) {
					return false;
				}
				values[i] = static_cast<float>(value);
			}
			a_result = ImVec2{ values[0], values[1] };
			return true;
		}
	}

	Style Load(const nlohmann::json& a_definition,
		const std::filesystem::path& a_directory)
	{
		Style result;
		result.LoadFailed = true;
		if (!a_definition.is_object() ||
			!ReadPair(a_definition, "Size", result.Size, 1.0F, 256.0F) ||
			!ReadPair(a_definition, "Hotspot", result.Hotspot, 0.0F, 1.0F)) {
			return result;
		}
		const auto path = a_definition.find("Image");
		if (path == a_definition.end() || !path->is_string()) {
			return result;
		}
		result.Image = LoadThemeImage(a_directory, path->get_ref<const std::string&>(), 512);
		result.LoadFailed = !result.Image;
		return result;
	}

	bool Draw(const Style& a_style, ImTextureID a_texture)
	{
		const auto& io = ImGui::GetIO();
		if (!io.MouseDrawCursor || !a_texture || !a_style.Image ||
			ImGui::GetMouseCursor() != ImGuiMouseCursor_Arrow ||
			!ImGui::IsMousePosValid(&io.MousePos)) {
			return false;
		}
		const float scale = ImGui::GetStyle().MouseCursorScale;
		if (!std::isfinite(scale) || scale <= 0.0F) {
			return false;
		}
		const ImVec2 size{ a_style.Size.x * scale, a_style.Size.y * scale };
		const ImVec2 topLeft{ io.MousePos.x - a_style.Hotspot.x * size.x,
			io.MousePos.y - a_style.Hotspot.y * size.y };
		ImGui::GetForegroundDrawList()->AddImage(a_texture, topLeft,
			ImVec2{ topLeft.x + size.x, topLeft.y + size.y });
		return true;
	}
}
