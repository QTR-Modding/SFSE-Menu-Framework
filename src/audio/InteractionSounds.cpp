#include "audio/InteractionSounds.h"
#include "WidgetAudio.h"
#include <imgui.h>
#include <imgui_internal.h>

namespace SFSEMenuFramework::Audio
{
    namespace
    {
        ImGuiContext* context{};
        ImGuiID previousHover{}, previousNavigation{};
        std::optional<Event> pending;

        bool Interactive(const ImGuiWindow& window)
        {
            return !window.IsFallbackWindow && !(window.Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup)) &&
                (window.Flags & ImGuiWindowFlags_NoInputs) != ImGuiWindowFlags_NoInputs;
        }

        void WidgetActivated(ImGuiAudio::Action action)
        {
            if (ImGui::GetCurrentContext() != context) return;
            const auto* window = ImGui::GetCurrentWindowRead();
            if (window && (window->Flags & ImGuiWindowFlags_NoInputs) == ImGuiWindowFlags_NoInputs) return;
            RequestInteraction(action == ImGuiAudio::Action::ToggleOn ? Event::ToggleOn :
                action == ImGuiAudio::Action::ToggleOff ? Event::ToggleOff : Event::Activate);
        }
    }

    void RequestInteraction(Event event)
    {
        if (context && (!pending || Priority(event) >= Priority(*pending))) pending = event;
    }

    void BeginInteractions(bool enabled)
    {
        pending.reset();
        context = enabled ? ImGui::GetCurrentContext() : nullptr;
        ImGuiAudio::Callback = context ? WidgetActivated : nullptr;
        if (!context) previousHover = previousNavigation = 0;
    }

    std::optional<Event> EndInteractions()
    {
        ImGuiAudio::Callback = nullptr;
        if (!context) return {};
        auto& g = *context;
        const auto hovered = g.NavDisableMouseHover || g.HoveredIdDisabled ? 0 : g.HoveredId;
        const auto navigated = g.NavDisableHighlight ? 0 : g.NavId;
        if (hovered && hovered != previousHover) RequestInteraction(Event::Hover);
        if (navigated && navigated != previousNavigation && g.NavJustMovedToId) RequestInteraction(Event::Navigate);
        previousHover = hovered;
        previousNavigation = navigated;
        if (g.NavWindow && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) RequestInteraction(Event::Back);
        for (const auto* window : g.Windows) {
            if (!Interactive(*window)) continue;
            if (window->Active && !window->WasActive) RequestInteraction(Event::Open);
            if (!window->Active && window->WasActive) RequestInteraction(Event::Close);
        }
        context = nullptr;
        return pending;
    }
}
