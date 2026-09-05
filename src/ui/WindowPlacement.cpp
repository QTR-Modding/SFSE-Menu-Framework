#include "ui/WindowPlacement.h"

#include "config/FrameworkSettings.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace SFSEMenuFramework::WindowPlacement
{
	namespace
	{
		// Adapted from SKSE Menu Framework PR #18, UI.cpp at
		// edea98b9e8cda213470f3c4b8fcb80fe8c89a50d (GPL-3.0).
		// Keep SFSE's existing client ImGui settings; migrate only these two windows.
		struct Placement final
		{
			const char* Name;
			const wchar_t* Section;
			float DefaultRatio;
			bool HasSavedState{};
			bool PendingSave{};
			ImVec2 Position{};
			ImVec2 Size{};
		};
		std::array placements{
			Placement{ "#MCPMainWindow", L"MainWindow", 0.8F },
			Placement{ "Settings##Window", L"SettingsWindow", 0.4F }
		};

		[[nodiscard]] bool ValidViewport(const ImGuiViewport& a_viewport)
		{
			return std::isfinite(a_viewport.Size.x) && std::isfinite(a_viewport.Size.y) &&
				a_viewport.Size.x > 0.0F && a_viewport.Size.y > 0.0F;
		}

		void Restore(Placement& a_state, const FrameworkSettings::WindowLayout& a_layout,
			const ImGuiViewport& a_viewport)
		{
			const auto minimum = ImGui::GetStyle().WindowMinSize;
			a_state.Size = ImVec2{
				std::round(std::clamp((std::min)(a_layout.Width, 1.0F) * a_viewport.Size.x,
					(std::min)(minimum.x, a_viewport.Size.x), a_viewport.Size.x)),
				std::round(std::clamp((std::min)(a_layout.Height, 1.0F) * a_viewport.Size.y,
					(std::min)(minimum.y, a_viewport.Size.y), a_viewport.Size.y))
			};
			a_state.Position = ImVec2{
				std::round(a_viewport.Pos.x + std::clamp(a_layout.X, 0.0F,
					(std::max)(0.0F, 1.0F - a_state.Size.x / a_viewport.Size.x)) * a_viewport.Size.x),
				std::round(a_viewport.Pos.y + std::clamp(a_layout.Y, 0.0F,
					(std::max)(0.0F, 1.0F - a_state.Size.y / a_viewport.Size.y)) * a_viewport.Size.y)
			};
			a_state.HasSavedState = true;
		}
	}

	void Apply(BuiltInWindow a_window)
	{
		auto& state = placements[static_cast<std::size_t>(a_window)];
		const auto& viewport = *ImGui::GetMainViewport();
		if (!ValidViewport(viewport)) {
			return;
		}
		if (!state.HasSavedState) {
			FrameworkSettings::WindowLayout layout{ state.Section };
			if (FrameworkSettings::LoadWindowLayout(layout)) {
				Restore(state, layout, viewport);
			} else if (const auto* previous = ImGui::FindWindowSettingsByID(ImHashStr(state.Name));
				previous && previous->Size.x > 0 && previous->Size.y > 0) {
				// ImGui stores pixel positions relative to their owning viewport.
				const auto savedOrigin = previous->ViewportId ? ImVec2{
					static_cast<float>(previous->ViewportPos.x), static_cast<float>(previous->ViewportPos.y) } : viewport.Pos;
				layout.X = (static_cast<float>(previous->Pos.x) + savedOrigin.x - viewport.Pos.x) / viewport.Size.x;
				layout.Y = (static_cast<float>(previous->Pos.y) + savedOrigin.y - viewport.Pos.y) / viewport.Size.y;
				layout.Width = static_cast<float>(previous->Size.x) / viewport.Size.x;
				layout.Height = static_cast<float>(previous->Size.y) / viewport.Size.y;
				Restore(state, layout, viewport);
				state.PendingSave = true;
			}
		}
		if (state.HasSavedState) {
			ImGui::SetNextWindowPos(state.Position, ImGuiCond_Appearing);
			ImGui::SetNextWindowSize(state.Size, ImGuiCond_Appearing);
		} else {
			ImGui::SetNextWindowPos(viewport.GetCenter(), ImGuiCond_Appearing, ImVec2{ 0.5F, 0.5F });
			ImGui::SetNextWindowSize(ImVec2{
				viewport.Size.x * state.DefaultRatio, viewport.Size.y * state.DefaultRatio },
				ImGuiCond_Appearing);
		}
	}

	void Capture(BuiltInWindow a_window)
	{
		auto& state = placements[static_cast<std::size_t>(a_window)];
		const auto position = ImGui::GetWindowPos();
		const auto size = ImGui::GetWindowSize();
		state.PendingSave |= state.HasSavedState &&
			(state.Position.x != position.x || state.Position.y != position.y ||
				state.Size.x != size.x || state.Size.y != size.y);
		state.Position = position;
		state.Size = size;
		state.HasSavedState = true;
	}

	void Reset()
	{
		const auto& viewport = *ImGui::GetMainViewport();
		if (!ValidViewport(viewport)) {
			return;
		}
		for (auto& state : placements) {
			const float margin = (1.0F - state.DefaultRatio) * 0.5F;
			Restore(state, { state.Section, margin, margin, state.DefaultRatio, state.DefaultRatio }, viewport);
			state.PendingSave = true;
			ImGui::SetWindowPos(state.Name, state.Position, ImGuiCond_Always);
			ImGui::SetWindowSize(state.Name, state.Size, ImGuiCond_Always);
		}
	}

	void SavePending(bool a_mainWindowOpen)
	{
		if (a_mainWindowOpen && (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
			ImGui::GetCurrentContext()->NavWindowingTarget)) {
			return;
		}
		const auto& viewport = *ImGui::GetMainViewport();
		if (!ValidViewport(viewport)) {
			return;
		}
		std::array<FrameworkSettings::WindowLayout, placements.size()> pending{};
		std::size_t count{};
		for (auto& state : placements) {
			if (!state.PendingSave) {
				continue;
			}
			// Retry failed writes on the next layout change, not every render frame.
			state.PendingSave = false;
			pending[count++] = FrameworkSettings::WindowLayout{
				state.Section,
				(state.Position.x - viewport.Pos.x) / viewport.Size.x,
				(state.Position.y - viewport.Pos.y) / viewport.Size.y,
				state.Size.x / viewport.Size.x,
				state.Size.y / viewport.Size.y
			};
		}
		if (count != 0 && !FrameworkSettings::SaveWindowLayouts({ pending.data(), count })) {
			logger::warn("Failed to save window layouts to SFSEMenuFramework.ini");
		}
	}
}
