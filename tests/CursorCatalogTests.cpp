#include "appearance/CursorManager.h"
#include "config/FrameworkSettings.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

bool TestCursorCatalog(const std::filesystem::path& a_fixture)
{
	using namespace SFSEMenuFramework;
	const auto directory = FrameworkSettings::BuildGamePath(
		L"Data/SFSE/Plugins/SFSEMenuFrameworkCursors");
	const auto ini = FrameworkSettings::BuildGamePath(L"Data/SFSE/Plugins/SFSEMenuFramework.ini");
	// These paths belong to the test executable, never Starfield. Do not touch
	// pre-existing fixtures or configuration from another run.
	if (directory.empty() || std::filesystem::exists(directory) || std::filesystem::exists(ini)) {
		std::cerr << "Cursor catalog fixture paths are not empty\n";
		return false;
	}
	std::filesystem::create_directories(directory);
	const auto png = directory / "ring.png";
	const auto metadata = directory / "ring.json";
	std::filesystem::copy_file(a_fixture, png);
	{ std::ofstream ignored{ directory / "ignored.txt" }; }
	bool passed = true;
	const auto check = [&](bool ok, const char* message) {
		if (!ok) {
			std::cerr << message << '\n';
			passed = false;
		}
	};
	FrameworkSettings::ResetDefaults();
	CursorManager::Update();
	check(CursorManager::GetCursors().size() == 1, "Only PNGs should appear in the cursor list");
	check(!CursorManager::GetImage() && !CursorManager::HasError(), "Default needs no image");
	check(FrameworkSettings::SetCursorName("ring.png"), "Cursor filename should be accepted");
	CursorManager::Update();
	const auto first = CursorManager::GetImage();
	check(first != nullptr && !CursorManager::HasError(), "Bare PNG should load without metadata");
	check(FrameworkSettings::SetMenuStyle("THE VOID"), "Theme change should be accepted");
	check(FrameworkSettings::SetCursorScale(1.75F), "Cursor scale should be accepted");
	CursorManager::Update();
	check(CursorManager::GetImage() == first, "Theme and size changes must not reload the cursor");
	check(!FrameworkSettings::SetCursorScale(0.0F), "Out-of-range scale must be rejected");
	check(!FrameworkSettings::SetCursorName("../ring.png"), "Non-leaf cursor names must be rejected");
	check(FrameworkSettings::Save(), "Cursor preferences should save");
	FrameworkSettings::ResetDefaults();
	check(FrameworkSettings::Load(), "Cursor preferences should load");
	check(std::string_view{ FrameworkSettings::GetCursorName().data() } == "RING.PNG" &&
		FrameworkSettings::GetCursorScale() == 1.75F, "Cursor name and scale must survive reload");
	check(std::string_view{ FrameworkSettings::GetMenuStyle().data() } == "THE VOID",
		"Theme preference must remain separate");

	{ std::ofstream file{ metadata }; file << "{invalid"; }
	CursorManager::Refresh();
	CursorManager::Update();
	check(!CursorManager::GetImage() && CursorManager::HasError(), "Invalid metadata must fall back");
	{ std::ofstream file{ metadata }; file << R"({"Hotspot":[0.5,0.5]})"; }
	CursorManager::Refresh();
	CursorManager::Update();
	check(CursorManager::GetImage() && !CursorManager::HasError(), "Refresh should reload repaired metadata");

	std::filesystem::remove(png);
	CursorManager::Refresh();
	CursorManager::Update();
	check(CursorManager::GetCursors().empty() && !CursorManager::GetImage() &&
		CursorManager::HasError(), "A removed selected cursor must fall back");
	std::filesystem::copy_file(a_fixture, png);
	CursorManager::Refresh();
	CursorManager::Update();
	check(CursorManager::GetCursors().size() == 1 && CursorManager::GetImage(),
		"Refresh must discover and load a newly added cursor");
	check(FrameworkSettings::SetCursorName("DEFAULT"), "Default selection should be accepted");
	CursorManager::Update();
	check(!CursorManager::GetImage() && !CursorManager::HasError(), "Default must release custom image state");

	std::filesystem::remove(png);
	std::filesystem::remove(metadata);
	std::filesystem::remove(directory / "ignored.txt");
	std::filesystem::remove(directory);
	std::filesystem::remove(ini);
	return passed;
}
