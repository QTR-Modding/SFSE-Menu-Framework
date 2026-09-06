#include "runtime/MenuPath.h"

#include <cstdio>
#include <string>

namespace
{
	int failures{};

	void Check(bool a_condition, const char* a_name)
	{
		if (!a_condition) {
			std::fprintf(stderr, "FAILED: %s\n", a_name);
			++failures;
		}
	}
}

int main()
{
	using SFSEMenuFramework::MenuPath::Parse;
	using SFSEMenuFramework::MenuPath::ParseSegment;

	const auto ordinary = Parse("Plugin/Settings/General");
	Check(
		ordinary && ordinary->size() == 3 &&
			(*ordinary)[0] == "Plugin" &&
			(*ordinary)[1] == "Settings" &&
			(*ordinary)[2] == "General",
		"ordinary nested path");

	const auto escaped = Parse("Plugin/Literal\\/slash/Leaf");
	Check(
		escaped && escaped->size() == 3 &&
			(*escaped)[1] == "Literal/slash",
		"escaped slash stays in one segment");

	const auto backslash = Parse("Plugin/Back\\slash");
	Check(
		backslash && backslash->size() == 2 &&
			(*backslash)[1] == "Back\\slash",
		"non-slash backslash is preserved");

	Check(!Parse(""), "empty path rejected");
	Check(!Parse("/Plugin"), "leading separator rejected");
	Check(!Parse("Plugin/"), "trailing separator rejected");
	Check(!Parse("Plugin//Leaf"), "empty segment rejected");
	Check(!Parse(std::string(1025, 'x')), "oversized path rejected");
	Check(Parse(std::string(1024, 'x')).has_value(), "maximum path accepted");

	const auto segment = ParseSegment("Literal\\/slash");
	Check(segment && *segment == "Literal/slash", "escaped rename segment");
	Check(!ParseSegment("Parent/Child"), "nested rename segment rejected");

	return failures == 0 ? 0 : 1;
}
