#pragma once
#include <imgui_internal.h>

// SKSE Menu Framework 3, 8fb2d295 (GPL-3.0-only).
namespace ImGui
{
    using ImGuiItemAddObserver = void (*)(ImGuiContext*, ImGuiWindow*, const ImGuiLastItemData*);
    IMGUI_API void SetItemAddObserver(ImGuiItemAddObserver observer);

    using ImGuiNavTweakProvider = float (*)(ImGuiAxis);
    IMGUI_API void SetNavTweakProvider(ImGuiNavTweakProvider provider);
}
