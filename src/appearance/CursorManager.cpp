#include "appearance/CursorManager.h"

#include "appearance/AssetDiscovery.h"
#include "appearance/CursorDrawing.h"
#include "config/FrameworkSettings.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <vector>

namespace SFSEMenuFramework::CursorManager
{
	namespace
	{
		struct State final
		{
			std::vector<Entry> Cursors;
			FrameworkSettings::MenuStyleName Name{};
			CursorDrawing::Style Active;
			std::uintptr_t Texture{};
			bool Initialized{};
			bool Reload{};
			bool UploadFailed{};
		};

		State& GetState()
		{
			static auto* state = new State();
			return *state;
		}

		CursorDrawing::Style Load(const std::filesystem::path& a_path)
		{
			auto metadataPath = a_path;
			metadataPath.replace_extension(".json");
			nlohmann::json definition = nlohmann::json::object();
			std::error_code error;
			const bool exists = std::filesystem::exists(metadataPath, error);
			if (error) {
				return { .LoadFailed = true };
			}
			if (exists) {
				const auto size = std::filesystem::file_size(metadataPath, error);
				if (error || size == 0 || size > 65536) {
					return { .LoadFailed = true };
				}
				std::ifstream stream{ metadataPath, std::ios::binary };
				definition = nlohmann::json::parse(stream, nullptr, false, false);
				if (!definition.is_object()) {
					return { .LoadFailed = true };
				}
			}
			// The discovered PNG owns identity; metadata cannot redirect its image.
			definition["Image"] = a_path.filename().string();
			return CursorDrawing::Load(definition, a_path.parent_path());
		}
	}

	void Refresh()
	{
		auto& state = GetState();
		Appearance::Detail::DiscoverFiles(
			L"Data/SFSE/Plugins/SFSEMenuFrameworkCursors", "cursor", true, state.Cursors,
			[](const std::filesystem::path& path, std::string& name) {
				return FrameworkSettings::NormalizeMenuStyleName(path.filename().wstring(), name) &&
					name.ends_with(".PNG");
			});
		state.Initialized = true;
		state.Reload = true;
	}

	void Update()
	{
		auto& state = GetState();
		if (!state.Initialized) {
			Refresh();
		}
		const auto name = FrameworkSettings::GetCursorName();
		if (!state.Reload && state.Name == name) {
			return;
		}
		state.Reload = false;
		state.Name = name;
		state.Active = {};
		if (std::string_view{ name.data() } == "DEFAULT") {
			return;
		}
		for (const auto& entry : state.Cursors) {
			if (entry.Name == name.data()) {
				state.Active = Load(entry.Path);
				return;
			}
		}
		state.Active.LoadFailed = true;
	}

	std::span<const Entry> GetCursors() noexcept { return GetState().Cursors; }
	std::shared_ptr<const ThemeImage> GetImage() noexcept { return GetState().Active.Image; }

	void SetTexture(std::uintptr_t a_texture, bool a_failed) noexcept
	{
		auto& state = GetState();
		state.Texture = a_texture;
		state.UploadFailed = a_failed;
	}

	bool HasError() noexcept { return GetState().Active.LoadFailed || GetState().UploadFailed; }

	bool Render()
	{
		const auto& state = GetState();
		return CursorDrawing::Draw(state.Active, reinterpret_cast<ImTextureID>(state.Texture));
	}
}
