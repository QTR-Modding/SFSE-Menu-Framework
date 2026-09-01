#pragma once

#include "config/FrameworkSettings.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace SFSEMenuFramework::Appearance::Detail
{
	[[nodiscard]] inline char LowerAscii(char a_character) noexcept
	{
		return a_character >= 'A' && a_character <= 'Z' ?
			static_cast<char>(a_character + ('a' - 'A')) : a_character;
	}

	[[nodiscard]] inline bool LessName(
		std::string_view a_left, std::string_view a_right,
		bool a_ignoreCase) noexcept
	{
		return std::lexicographical_compare(
			a_left.begin(), a_left.end(), a_right.begin(), a_right.end(),
			[a_ignoreCase](char a_lhs, char a_rhs) {
				return (a_ignoreCase ? LowerAscii(a_lhs) : a_lhs) <
					(a_ignoreCase ? LowerAscii(a_rhs) : a_rhs);
			});
	}

	template <class Entry, class ReadName>
	void DiscoverFiles(
		std::wstring_view a_relativeDirectory, std::string_view a_kind,
		bool a_ignoreCase, std::vector<Entry>& a_entries, ReadName a_readName)
	{
		a_entries.clear();
		const auto directory = FrameworkSettings::BuildGamePath(a_relativeDirectory);
		if (directory.empty()) {
			logger::warn("Could not resolve the SFSE Menu Framework {} directory", a_kind);
			return;
		}

		std::error_code error;
		std::filesystem::directory_iterator iterator{ directory, error }, end;
		for (; !error && iterator != end; iterator.increment(error)) {
			std::error_code entryError;
			std::string name;
			if (iterator->is_regular_file(entryError) && !entryError &&
				a_readName(iterator->path(), name)) {
				a_entries.push_back({ std::move(name), iterator->path() });
			}
		}
		if (error) {
			logger::warn("Could not fully enumerate the {} directory: {}", a_kind,
				error.message());
		}

		const auto same = [a_ignoreCase](const Entry& a_left, const Entry& a_right) {
			return a_ignoreCase ? FrameworkSettings::EqualsIgnoreCaseAscii(
				a_left.Name, a_right.Name) : a_left.Name == a_right.Name;
		};
		std::sort(a_entries.begin(), a_entries.end(),
			[a_ignoreCase, &same](const Entry& a_left, const Entry& a_right) {
				return same(a_left, a_right) ?
					a_left.Path.native() < a_right.Path.native() :
					LessName(a_left.Name, a_right.Name, a_ignoreCase);
			});
		a_entries.erase(std::unique(a_entries.begin(), a_entries.end(), same),
			a_entries.end());
	}
}
