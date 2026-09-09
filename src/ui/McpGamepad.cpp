#include "ui/McpGamepad.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <optional>

// Selectively adapts SKSE Menu Framework's RequestFocus, FocusWindowWithoutItem,
// RenderFocusedItemHighlight and hint-bar layout at
// 8fb2d295aee582a24204b015f39214ad43717728 (GPL-3.0).
// ImGui retains widget navigation and remembered focus; no item observer or
// cross-frame list of client widget pointers is installed.
namespace SFSEMenuFramework::McpGamepad
{
    namespace
    {
        ImGuiWindow* mainWindow{}; // Valid only during this render pass.
        bool enabled{}, hasPage{};
        std::optional<Area> requested;
        int lastFrame{-1};
        bool firstItem{};
        bool inContent{};

        bool Inside(const ImGuiWindow* window, const ImGuiWindow* ancestor)
        {
            for (auto* current = window; current; current = current->ParentWindowInBeginStack)
                if (current == ancestor) return true;
            return false;
        }

        bool CanNavigate()
        {
            const auto& g = *ImGui::GetCurrentContext();
            return enabled && g.NavWindow && Inside(g.NavWindow, mainWindow) &&
                g.OpenPopupStack.empty() && !g.NavWindowingTarget &&
                !ImGui::IsKeyDown(ImGuiKey_GamepadFaceLeft);
        }

        const char* Hints()
        {
            return hasPage ?
                "A: Select   B: Back   RB: Switch panel   Hold X: Move / resize" :
                "A: Select   B: Back   Hold X: Move / resize";
        }
    }

    void Begin(bool gamepad, bool page)
    {
        const int frame = ImGui::GetFrameCount();
        if (frame != lastFrame + 1 || !gamepad) requested.reset();
        lastFrame = frame;
        mainWindow = ImGui::GetCurrentWindow();
        enabled = gamepad;
        hasPage = page;
        if (!CanNavigate()) {
            requested.reset();
            return;
        }
        if (!hasPage && inContent) {
            requested = Area::Tree;
            firstItem = false;
        }
        if (hasPage && !ImGui::IsAnyItemActive() &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
            requested = inContent ? Area::Tree : Area::Content;
            firstItem = false;
        }
    }

    void RequestPageFocus()
    {
        if (!CanNavigate()) return;
        requested = Area::Content;
        firstItem = true;
    }

    void BeginArea(Area area)
    {
        auto* window = ImGui::GetCurrentWindow();
        if (Inside(ImGui::GetCurrentContext()->NavWindow, window))
            inContent = area == Area::Content;
        if (!requested || *requested != area || !CanNavigate() ||
            ImGui::IsAnyItemActive() || ImGui::IsKeyDown(ImGuiKey_GamepadFaceDown)) return;
        if (area == Area::Content && !hasPage) return;

        ImGui::NavMoveRequestCancel();
        ImGui::FocusWindow(window, ImGuiFocusRequestFlags_UnlessBelowModal |
            (firstItem ? 0 : ImGuiFocusRequestFlags_RestoreFocusedChild));
        auto& g = *ImGui::GetCurrentContext();
        ImGui::NavInitWindow(g.NavWindow, firstItem);
        g.NavInputSource = ImGuiInputSource_Gamepad;
        g.NavDisableHighlight = false;
        g.NavDisableMouseHover = true;
        inContent = area == Area::Content;
        requested.reset();
    }

    float GetFooterHeight()
    {
        if (!enabled) return 0.0F;
        return ImGui::CalcTextSize(Hints(), nullptr, false,
            (std::max)(1.0F, ImGui::GetContentRegionAvail().x)).y +
            ImGui::GetStyle().ItemSpacing.y * 2.0F;
    }

    void End()
    {
        if (CanNavigate()) {
            auto& g = *ImGui::GetCurrentContext();
            auto* window = g.NavWindow;
            if (g.NavId && !g.NavDisableHighlight) {
                auto rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[g.NavLayer]);
                rect.Expand(2.0F);
                rect.ClipWith(window->ClipRect);
                if (rect.GetWidth() > 0.0F && rect.GetHeight() > 0.0F) {
                    auto accent = ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight);
                    accent.w = (std::max)(accent.w, 0.85F);
                    auto* draw = window->DrawList;
                    draw->PushClipRect(window->ClipRect.Min, window->ClipRect.Max, true);
                    draw->AddRect(rect.Min, rect.Max, ImGui::GetColorU32(accent),
                        ImGui::GetStyle().FrameRounding, 0, g.ActiveId == g.NavId ? 3.0F : 2.0F);
                    draw->PopClipRect();
                }
            }
        }
        if (enabled) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", Hints());
        }
        mainWindow = nullptr;
    }
}
