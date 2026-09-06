#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SFSEMenuFramework::MenuPath
{
	using Segments = std::vector<std::string>;

	[[nodiscard]] std::optional<Segments> Parse(std::string_view a_path);
	[[nodiscard]] std::optional<std::string> ParseSegment(
		std::string_view a_segment);
}
