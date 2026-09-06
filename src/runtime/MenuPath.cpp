#include "runtime/MenuPath.h"

#include <utility>

namespace SFSEMenuFramework::MenuPath
{
	namespace
	{
		constexpr std::size_t maximumPathLength = 1024;
	}

	std::optional<Segments> Parse(std::string_view a_path)
	{
		// This parser adapts SKSE Menu Framework 3 MenuPath.cpp at commit
		// c8cfc5c93fa3b5f6261cef695ab814e4467dd980 (GPL-3.0).
		if (a_path.empty() || a_path.size() > maximumPathLength) {
			return std::nullopt;
		}

		Segments segments;
		std::string segment;
		segment.reserve(a_path.size());

		for (std::size_t index = 0; index < a_path.size(); ++index) {
			const char character = a_path[index];
			if (character == '\\' && index + 1 < a_path.size() &&
				a_path[index + 1] == '/') {
				segment.push_back('/');
				++index;
				continue;
			}

			if (character == '/') {
				if (segment.empty()) {
					return std::nullopt;
				}
				segments.push_back(std::move(segment));
				segment.clear();
				continue;
			}

			segment.push_back(character);
		}

		if (segment.empty()) {
			return std::nullopt;
		}
		segments.push_back(std::move(segment));
		return segments;
	}

	std::optional<std::string> ParseSegment(std::string_view a_segment)
	{
		auto parsed = Parse(a_segment);
		if (!parsed || parsed->size() != 1) {
			return std::nullopt;
		}
		return std::move(parsed->front());
	}
}
