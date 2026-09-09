#include "appearance/CursorDrawing.h"

#include <Windows.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

bool TestCursorCatalog(const std::filesystem::path&);

namespace
{
	int failures{};
	void Check(bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << a_message << '\n';
			++failures;
		}
	}
}

int main()
{
	using namespace SFSEMenuFramework;
	using nlohmann::json;
	// Generated 2x2 RGBA fixture, with one white pixel and three transparent pixels.
	const unsigned char png[]{
		0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
		0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
		0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
		0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c, 0xe9, 0x00, 0x00,
		0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc,
		0x61, 0x05, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00,
		0x0e, 0xc3, 0x00, 0x00, 0x0e, 0xc3, 0x01, 0xc7, 0x6f, 0xa8, 0x64, 0x00,
		0x00, 0x00, 0x10, 0x49, 0x44, 0x41, 0x54, 0x18, 0x57, 0x63, 0xf8, 0xff,
		0xff, 0xff, 0x7f, 0x06, 0x64, 0x00, 0x00, 0x3d, 0xd4, 0x03, 0xfd, 0x01,
		0xb5, 0x78, 0xcb, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
		0x42, 0x60, 0x82,
	};
	const auto directory = std::filesystem::temp_directory_path() /
		("sfse-cursor-" + std::to_string(::GetCurrentProcessId()) + "-" +
			std::to_string(::GetTickCount64()));
	if (!std::filesystem::create_directory(directory)) {
		return 1;
	}
	{
		std::ofstream image{ directory / "ring.png", std::ios::binary };
		image.write(reinterpret_cast<const char*>(png), sizeof(png));
	}

	const CursorDrawing::Style empty;
	Check(!empty.Image && !empty.LoadFailed, "Absent cursor must use the default without an error");
	json theme{ { "Image", "ring.png" } };
	const auto basic = CursorDrawing::Load(theme, directory);
	Check(basic.Image && !basic.LoadFailed, "Valid PNG should load");
	Check(basic.Size.x == 32 && basic.Hotspot.x == 0, "Defaults should be 32px and top-left");
	Check(basic.Image && basic.Image->Pixels.size() == 16 &&
		basic.Image->Pixels[3] == 255 && basic.Image->Pixels[7] == 0, "PNG alpha must survive decoding");
	Check(!LoadThemeImage(directory, "ring.png", 1), "Dimension limit must be applied before decoding");
	for (const auto* path : { "../ring.png", "/ring.png", "C:/ring.png", "missing.png" }) {
		auto invalid = theme;
		invalid["Image"] = path;
		const auto result = CursorDrawing::Load(invalid, directory);
		Check(result.LoadFailed && !result.Image, "Invalid path must fall back");
	}
	for (const auto& invalidCursor : { json{}, json{ "ring.png" }, json::object() }) {
		const auto result = CursorDrawing::Load(invalidCursor, directory);
		Check(result.LoadFailed && !result.Image, "Invalid Cursor object must fall back");
	}
	for (const auto& invalidSize : { json{ 0, 32 }, json{ 257, 32 }, json{ "32", 32 },
		json{ 32 }, json{ 32, 32, 32 }, json{ -1, 32 },
		json{ std::numeric_limits<double>::infinity(), 32 } }) {
		auto invalid = theme;
		invalid["Size"] = invalidSize;
		const auto result = CursorDrawing::Load(invalid, directory);
		Check(result.LoadFailed && !result.Image, "Invalid size must fall back");
	}
	for (const auto& invalidHotspot : { json{ -0.1, 0.5 }, json{ 0.5, 1.1 }, json{ 0.5 } }) {
		auto invalid = theme;
		invalid["Hotspot"] = invalidHotspot;
		Check(CursorDrawing::Load(invalid, directory).LoadFailed, "Invalid hotspot must fall back");
	}
	theme["Size"] = { 20, 30 };
	theme["Hotspot"] = { 0.5, 0.5 };
	const auto cursor = CursorDrawing::Load(theme, directory);
	Check(cursor.Image && !cursor.LoadFailed, "Custom size and center hotspot should load");

	ImGui::CreateContext();
	auto& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = ImVec2{ 640, 480 };
	io.MousePos = ImVec2{ 50, 60 };
	io.MouseDrawCursor = true;
	io.Fonts->AddFontDefault();
	unsigned char* pixels{};
	int width{}, height{};
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	ImGui::NewFrame();
	ImGui::GetStyle().MouseCursorScale = 2;
	const auto texture = reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(1));
	Check(!CursorDrawing::Draw(cursor, nullptr), "Failed upload must use the normal cursor");
	Check(!CursorDrawing::Draw(empty, texture), "Theme without a cursor must use the normal cursor");
	for (const auto shape : { ImGuiMouseCursor_TextInput, ImGuiMouseCursor_ResizeEW,
		ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_None }) {
		ImGui::SetMouseCursor(shape);
		Check(!CursorDrawing::Draw(cursor, texture), "Special cursor shapes must remain unchanged");
	}
	ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
	io.MouseDrawCursor = false;
	Check(!CursorDrawing::Draw(cursor, texture), "Hidden/gamepad cursor must stay hidden");
	io.MouseDrawCursor = true;
	Check(CursorDrawing::Draw(cursor, texture), "Normal pointer should use the custom image");
	const auto* drawList = ImGui::GetForegroundDrawList();
	Check(drawList->VtxBuffer.Size == 4, "Custom cursor should draw one quad");
	if (drawList->VtxBuffer.Size == 4) {
		Check(drawList->VtxBuffer[0].pos.x == 30 && drawList->VtxBuffer[0].pos.y == 30 &&
			drawList->VtxBuffer[2].pos.x == 70 && drawList->VtxBuffer[2].pos.y == 90,
			"Size and hotspot must scale together around the click position");
	}
	Check(io.MouseDrawCursor, "Drawing must not change input cursor ownership");
	io.MouseDrawCursor = false;
	ImGui::Render();
	ImGui::DestroyContext();
	Check(TestCursorCatalog(directory / "ring.png"), "Cursor catalog and persistence");
	std::filesystem::remove_all(directory);
	if (!failures) {
		std::cout << "Cursor tests passed\n";
	}
	return failures ? 1 : 0;
}
