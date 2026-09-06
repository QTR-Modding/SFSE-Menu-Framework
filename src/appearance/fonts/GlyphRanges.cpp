#include "appearance/fonts/GlyphRanges.h"

namespace SFSEMenuFramework::Fonts
{
	namespace
	{
		constexpr ImWchar turkishGlyphRanges[]{
			0x011E, 0x011F,
			0x0130, 0x0131,
			0x015E, 0x015F,
			0
		};
		constexpr ImWchar polishGlyphRanges[]{
			0x00D3, 0x00D3,
			0x00F3, 0x00F3,
			0x0104, 0x0107,
			0x0118, 0x0119,
			0x0141, 0x0144,
			0x015A, 0x015B,
			0x0179, 0x017C,
			0
		};
		constexpr ImWchar iconGlyphRanges[]{ 0xE005, 0xF8FF, 0 };
	}

	void BuildTextGlyphRanges(
		ImFontAtlas& a_atlas,
		const FrameworkSettings::GlyphCoverage& a_coverage,
		std::vector<ImWchar>& a_ranges)
	{
		// The configurable range union plus Turkish and Polish additions adapt
		// SKSE Menu Framework 3 src/FontManager.cpp through commit
		// c8cfc5c93fa3b5f6261cef695ab814e4467dd980 (GPL-3.0).
		// Greek, Vietnamese, and separate Simplified/Full Chinese choices are
		// safe extensions using Dear ImGui's built-in range tables.
		ImFontGlyphRangesBuilder builder;
		builder.AddRanges(a_atlas.GetGlyphRangesDefault());
		if (a_coverage.Greek) {
			builder.AddRanges(a_atlas.GetGlyphRangesGreek());
		}
		if (a_coverage.Cyrillic) {
			builder.AddRanges(a_atlas.GetGlyphRangesCyrillic());
		}
		if (a_coverage.Vietnamese) {
			builder.AddRanges(a_atlas.GetGlyphRangesVietnamese());
		}
		if (a_coverage.Turkish) {
			builder.AddRanges(turkishGlyphRanges);
		}
		if (a_coverage.Polish) {
			builder.AddRanges(polishGlyphRanges);
		}
		if (a_coverage.Thai) {
			builder.AddRanges(a_atlas.GetGlyphRangesThai());
		}
		if (a_coverage.Korean) {
			builder.AddRanges(a_atlas.GetGlyphRangesKorean());
		}
		if (a_coverage.Japanese) {
			builder.AddRanges(a_atlas.GetGlyphRangesJapanese());
		}
		if (a_coverage.ChineseSimplifiedCommon) {
			builder.AddRanges(a_atlas.GetGlyphRangesChineseSimplifiedCommon());
		} else if (a_coverage.ChineseFull) {
			builder.AddRanges(a_atlas.GetGlyphRangesChineseFull());
		}
		ImVector<ImWchar> built;
		builder.BuildRanges(&built);
		a_ranges.assign(built.begin(), built.end());
	}

	const ImWchar* GetIconGlyphRanges() noexcept
	{
		return iconGlyphRanges;
	}
}
