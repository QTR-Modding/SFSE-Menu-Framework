#include "localization/Translations.h"
#include "ui/CursorSettings.h"

#include "appearance/CursorManager.h"
#include "config/FrameworkSettings.h"

#include <imgui.h>
#include <optional>
#include <string_view>

namespace SFSEMenuFramework::CursorSettings
{
	namespace
	{
		std::optional<float> originalScale;
	}

	void FinishEdit(bool& a_saveFailed) noexcept
	{
		if (!originalScale) {
			return;
		}
		a_saveFailed = !FrameworkSettings::Save();
		if (a_saveFailed) {
			static_cast<void>(FrameworkSettings::SetCursorScale(*originalScale));
		}
		originalScale.reset();
	}

	void Render(bool& a_saveFailed)
	{
		const auto name = FrameworkSettings::GetCursorName();
		ImGui::TextUnformatted(SFSEMenuFramework::Translations::Get("Cursor", "Cursor"));
		if (ImGui::BeginCombo("##Cursor", std::string_view{ name.data() } == "DEFAULT" ?
			Translations::Get("Default", "Default") : name.data())) {
			const auto choose = [&](const char* label, const char* value) {
				const bool selected = std::string_view{ name.data() } == value;
				if (ImGui::Selectable(label, selected) && FrameworkSettings::SetCursorName(value)) {
					a_saveFailed = !FrameworkSettings::Save();
					if (a_saveFailed) {
						static_cast<void>(FrameworkSettings::SetCursorName(name.data()));
					}
				}
				if (selected) {
					ImGui::SetItemDefaultFocus();
				}
			};
			choose(Translations::Get("Default", "Default"), "DEFAULT");
			for (const auto& entry : CursorManager::GetCursors()) {
				choose(entry.Name.c_str(), entry.Name.c_str());
			}
			ImGui::EndCombo();
		}
		ImGui::TextUnformatted(SFSEMenuFramework::Translations::Get("Cursor size", "Cursor size"));
		auto scale = FrameworkSettings::GetCursorScale();
		const auto previous = scale;
		if (ImGui::SliderFloat("##CursorSize", &scale, 0.5F, 3.0F, "%.2fx",
			ImGuiSliderFlags_AlwaysClamp)) {
			if (!originalScale) {
				originalScale = previous;
			}
			static_cast<void>(FrameworkSettings::SetCursorScale(scale));
		}
		if (!ImGui::IsItemActive()) {
			FinishEdit(a_saveFailed);
		}
		if (ImGui::Button(SFSEMenuFramework::Translations::Get("Refresh cursors", "Refresh cursors"))) {
			CursorManager::Refresh();
		}
		if (CursorManager::HasError()) {
			ImGui::TextWrapped(SFSEMenuFramework::Translations::Get("Could not load the selected cursor. Using the default pointer.", "Could not load the selected cursor. Using the default pointer."));
		}
	}
}
