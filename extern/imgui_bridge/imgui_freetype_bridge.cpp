// Dear ImGui 1.90.8-docking misc/freetype/imgui_freetype.cpp is compiled through
// this narrow bridge so SFSE Menu Framework can select an OpenType variation
// weight without changing any public ImGui structure or consumer ABI.
// Dear ImGui source commit: 6d948ab47ecf984239af01434f3ed03808dbf188 (MIT).

#include "FontVariation.h"

#include <imgui.h>

#if IMGUI_VERSION_NUM != 19080
#error Review the FreeType variable-font bridge before updating Dear ImGui.
#endif

namespace
{
	[[nodiscard]] FT_Error OpenMemoryFaceWithRequestedWeight(
		FT_Library      a_library,
		const FT_Byte*  a_bytes,
		FT_Long         a_size,
		FT_Long         a_faceIndex,
		FT_Face*        a_face) noexcept
	{
		if (!a_library || !a_bytes || a_size <= 0 || !a_face) {
			return FT_Err_Invalid_Argument;
		}
		*a_face = nullptr;
		const auto error = FT_New_Memory_Face(
			a_library,
			a_bytes,
			a_size,
			a_faceIndex,
			a_face);
		if (error != 0) {
			*a_face = nullptr;
			return error;
		}
		if (!*a_face) {
			return FT_Err_Invalid_Argument;
		}
		if (SFSEMenuFramework::FontVariation::ApplyRequestedWeight(
				a_library,
				*a_face,
				std::span{
					reinterpret_cast<const std::uint8_t*>(a_bytes),
					static_cast<std::size_t>(a_size) })) {
			return 0;
		}

		static_cast<void>(FT_Done_Face(*a_face));
		*a_face = nullptr;
		return FT_Err_Invalid_Argument;
	}
}

#define FT_New_Memory_Face OpenMemoryFaceWithRequestedWeight
#include "../imgui/misc/freetype/imgui_freetype.cpp"
#undef FT_New_Memory_Face
