#include "ui/McpGamepad.h"
#include "appearance/GamepadIcons.h"
#include "ItemObserver.h"

// Ported from SKSE Menu Framework 3, GamepadNavigation.cpp at
// 8fb2d295aee582a24204b015f39214ad43717728 (GPL-3.0-only).

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

namespace SFSEMenuFramework::McpGamepad {
    namespace {
        constexpr std::size_t kAreaCount = static_cast<std::size_t>(Area::Count);
        constexpr float kScrollSpeed = 900.0f;

        struct NavigableItem {
            ImGuiID Id = 0;
            ImGuiWindow* Window = nullptr;
            ImGuiID FocusScopeId = 0;
            ImGuiNavLayer Layer = ImGuiNavLayer_Main;
            ImRect Rect{};
        };

        using IconSlot = GamepadIcons::Slot;

        struct Hint {
            IconSlot PrimaryIcon;
            IconSlot SecondaryIcon;
            const char* Text;
        };

        std::array<std::vector<NavigableItem>, kAreaCount> previousItems;
        std::array<std::vector<NavigableItem>, kAreaCount> currentItems;
        std::array<std::size_t, kAreaCount> focusedIndices{};
        std::array<ImGuiWindow*, kAreaCount> areaWindows{};


        Area activeArea = Area::PageTree;
        Area areaBeforeOptions = Area::PageTree;
        Area recordingArea = Area::PageTree;
        std::optional<Area> requestedFocus;

        ImGuiWindow* panelWindow = nullptr;
        ImVec4 focusAccent{};
        bool gamepadActive = false;
        bool gamepadBecameActive = false;
        bool suspended = false;
        bool wasSuspended = false;
        bool recordingItems = false;
        bool optionsVisible = false;
        bool optionsNeedFocus = false;
        bool optionsToggleRequested = false;
        bool focusStylePushed = false;
        int lastRenderedFrame = -1;

        std::size_t AreaIndex(Area area) { return static_cast<std::size_t>(area); }

        bool IsPanelNavigationActive() { return gamepadActive && !suspended; }

        bool IsWindowInside(const ImGuiWindow* window, const ImGuiWindow* ancestor) {
            for (const ImGuiWindow* current = window; current; current = current->ParentWindowInBeginStack) {
                if (current == ancestor) {
                    return true;
                }
            }
            return false;
        }

        bool IsChildContainer(const ImGuiContext* context, const ImGuiWindow* parent, ImGuiID id) {
            for (ImGuiWindow* candidate : context->Windows) {
                if (candidate->ParentWindow == parent && candidate->ChildId == id) {
                    return true;
                }
            }
            return false;
        }

        void ObserveItem(ImGuiContext* context, ImGuiWindow* window, const ImGuiLastItemData* itemData) {
            if (!IsPanelNavigationActive() || !recordingItems || !context || !window || !itemData ||
                itemData->ID == 0) {
                return;
            }
            if (itemData->InFlags & (ImGuiItemFlags_NoNav | ImGuiItemFlags_Disabled)) {
                return;
            }
            if (window->Flags & ImGuiWindowFlags_Tooltip) {
                return;
            }
            if ((window->Flags & ImGuiWindowFlags_Popup) &&
                (recordingArea != Area::OptionsMenu || window != areaWindows[AreaIndex(Area::OptionsMenu)])) {
                return;
            }
            if (itemData->NavRect.GetWidth() <= 0.0f || itemData->NavRect.GetHeight() <= 0.0f) {
                return;
            }
            if (IsChildContainer(context, window, itemData->ID)) {
                return;
            }

            auto& items = currentItems[AreaIndex(recordingArea)];
            const auto duplicate = std::find_if(items.begin(), items.end(), [&](const NavigableItem& item) {
                return item.Id == itemData->ID && item.Window == window;
            });
            if (duplicate != items.end()) {
                return;
            }

            items.push_back(
                {itemData->ID, window, context->CurrentFocusScopeId, window->DC.NavLayerCurrent, itemData->NavRect});
        }

        void CancelDefaultMoveRequest() {
            ImGuiContext& context = *GImGui;
            if (context.NavMoveSubmitted || context.NavMoveScoringItems) {
                ImGui::NavMoveRequestCancel();
            }
        }

        bool IsPressed(ImGuiKey dpadKey, ImGuiKey stickKey) {
            return ImGui::IsKeyPressed(dpadKey, true) || ImGui::IsKeyPressed(stickKey, true);
        }

        bool IsPopupBlockingAreaNavigation() {
            return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        }

        bool IsPopupBlockingAreaNavigation(Area area) {
            if (!IsPopupBlockingAreaNavigation()) {
                return false;
            }
            if (area != Area::OptionsMenu) {
                return true;
            }

            ImGuiWindow* navWindow = GImGui->NavWindow;
            ImGuiWindow* optionsWindow = areaWindows[AreaIndex(Area::OptionsMenu)];
            return navWindow && (navWindow->Flags & ImGuiWindowFlags_Popup) && navWindow != optionsWindow;
        }

        void FocusWindowWithoutItem(ImGuiWindow* window) {
            if (!window) {
                return;
            }

            ImGuiContext& context = *GImGui;
            ImGui::FocusWindow(window, ImGuiFocusRequestFlags_UnlessBelowModal);
            ImGui::NavInitWindow(window, true);
            context.NavInputSource = ImGuiInputSource_Gamepad;
            context.NavDisableHighlight = false;
        }

        void FocusItem(const NavigableItem& item) {
            if (!item.Window || (!item.Window->Active && !item.Window->WasActive)) {
                return;
            }

            ImGuiContext& context = *GImGui;
            ImGui::FocusWindow(item.Window, ImGuiFocusRequestFlags_UnlessBelowModal);
            ImGui::SetNavID(item.Id, item.Layer, item.FocusScopeId, ImGui::WindowRectAbsToRel(item.Window, item.Rect));
            ImGui::ScrollToRectEx(item.Window, item.Rect,
                                  ImGuiScrollFlags_KeepVisibleEdgeX | ImGuiScrollFlags_KeepVisibleEdgeY);
            context.NavInputSource = ImGuiInputSource_Gamepad;
            context.NavDisableHighlight = false;
        }

        std::optional<std::size_t> FindFocusedItem(const std::vector<NavigableItem>& items) {
            const ImGuiContext& context = *GImGui;
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (items[index].Id == context.NavId && items[index].Window == context.NavWindow) {
                    return index;
                }
            }
            return std::nullopt;
        }

        void MoveSequentially(Area area, int direction) {
            const std::size_t areaIndex = AreaIndex(area);
            const auto& items = previousItems[areaIndex];
            if (items.empty()) {
                requestedFocus = area;
                return;
            }

            const auto focusedItem = FindFocusedItem(items);
            std::size_t index = focusedItem.value_or(std::min(focusedIndices[areaIndex], items.size() - 1));
            if (direction < 0) {
                index = index == 0 ? items.size() - 1 : index - 1;
            } else {
                index = (index + 1) % items.size();
            }

            focusedIndices[areaIndex] = index;
            FocusItem(items[index]);
        }

        void ProcessAreaInput(Area area) {
            if (activeArea != area || IsPopupBlockingAreaNavigation(area) || ImGui::IsAnyItemActive()) {
                return;
            }

            const bool previous = IsPressed(ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp);
            const bool next = IsPressed(ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown);
            if (previous == next) {
                return;
            }

            CancelDefaultMoveRequest();
            MoveSequentially(area, previous ? -1 : 1);
        }

        void ScrollArea(Area area) {
            if (activeArea != area || IsPopupBlockingAreaNavigation(area)) {
                return;
            }

            const float direction = (ImGui::IsKeyDown(ImGuiKey_GamepadRStickDown) ? 1.0f : 0.0f) -
                                    (ImGui::IsKeyDown(ImGuiKey_GamepadRStickUp) ? 1.0f : 0.0f);
            if (direction == 0.0f) {
                return;
            }

            ImGuiWindow* window = GImGui->NavWindow;
            const std::size_t areaIndex = AreaIndex(area);
            if (!window || !IsWindowInside(window, areaWindows[areaIndex])) {
                window = areaWindows[areaIndex];
            }
            if (!window) {
                return;
            }

            const float deltaTime = ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f;
            ImGui::SetScrollY(window, window->Scroll.y + direction * kScrollSpeed * deltaTime);
        }

        std::vector<Hint> GetHints(bool hasPage) {
            constexpr IconSlot noSecondaryIcon = IconSlot::Count;
            switch (activeArea) {
                case Area::OptionsMenu:
                case Area::Popup:
                    return {
                        {IconSlot::Up, IconSlot::Down, "Navigate"},
                        {IconSlot::Confirm, noSecondaryIcon, "Select"},
                        {IconSlot::Cancel, noSecondaryIcon, "Back"},
                    };
                case Area::PageContent:
                    return {
                        {IconSlot::Up, IconSlot::Down, "Navigate"},
                        {IconSlot::Left, IconSlot::Right, "Adjust"},
                        {IconSlot::Confirm, noSecondaryIcon, "Select"},
                        {IconSlot::RightStick, noSecondaryIcon, "Scroll"},
                        {IconSlot::Cancel, noSecondaryIcon, "Back"},
                    };
                case Area::PageTree: {
                    std::vector<Hint> hints{
                        {IconSlot::Up, IconSlot::Down, "Navigate"},
                        {IconSlot::Confirm, noSecondaryIcon, "Select"},
                        {IconSlot::Options, noSecondaryIcon, "Options"},
                    };
                    if (hasPage) {
                        hints.push_back({IconSlot::RightShoulder, noSecondaryIcon, "Controls"});
                    }
                    hints.push_back({IconSlot::Cancel, noSecondaryIcon, "Back"});
                    return hints;
                }
                case Area::Count:
                    break;
            }
            return {};
        }

        float HintWidth(const Hint& hint, float iconSize) {
            float width = iconSize;
            if (hint.SecondaryIcon != IconSlot::Count) {
                width += iconSize + ImGui::GetStyle().ItemInnerSpacing.x;
            }
            width += ImGui::GetStyle().ItemInnerSpacing.x;
            width += ImGui::CalcTextSize(hint.Text).x;
            return width;
        }

        void RenderIcon(IconSlot slot, float size) {
            ImGui::Image(GamepadIcons::GetTexture(slot), ImVec2(size, size));
        }

        void RenderHint(const Hint& hint, float iconSize) {
            ImGui::BeginGroup();
            RenderIcon(hint.PrimaryIcon, iconSize);
            if (hint.SecondaryIcon != IconSlot::Count) {
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                RenderIcon(hint.SecondaryIcon, iconSize);
            }
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(hint.Text);
            ImGui::EndGroup();
        }
    }

    void BeginFrame(bool hasPage, bool shouldSuspend) {
        ImGui::SetItemAddObserver(ObserveItem);

        panelWindow = ImGui::GetCurrentWindow();
        focusAccent = ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight);
        suspended = shouldSuspend;
        optionsVisible = false;
        optionsToggleRequested = false;
        recordingItems = false;
        areaWindows.fill(nullptr);
        for (auto& items : currentItems) {
            items.clear();
        }

        const int frame = ImGui::GetFrameCount();
        const bool firstFrameAfterOpen = lastRenderedFrame < 0 || frame != lastRenderedFrame + 1;
        lastRenderedFrame = frame;

        if (!hasPage) {
            previousItems[AreaIndex(Area::PageContent)].clear();
            focusedIndices[AreaIndex(Area::PageContent)] = 0;
            if (activeArea == Area::PageContent) {
                activeArea = Area::PageTree;
            }
        }

        if (!IsPanelNavigationActive()) {
            wasSuspended = suspended;
            return;
        }

        if (firstFrameAfterOpen || gamepadBecameActive || (wasSuspended && !suspended)) {
            activeArea = Area::PageTree;
            requestedFocus = Area::PageTree;
            gamepadBecameActive = false;
        }
        wasSuspended = suspended;

        optionsToggleRequested = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) && !ImGui::IsAnyItemActive();

        if (!IsPopupBlockingAreaNavigation() && !ImGui::IsAnyItemActive()) {
            if (hasPage && ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
                RequestFocus(Area::PageContent);
            }
        }
    }

    void EndFrame() {
        recordingItems = false;

        if (IsPanelNavigationActive() && activeArea == Area::OptionsMenu && !optionsVisible) {
            activeArea = areaBeforeOptions;
            requestedFocus = activeArea;
        }

        ImGui::SetItemAddObserver(nullptr);
    }

    void NotifyInputDevice(bool gamepad) {
        if (gamepad) {
            if (!gamepadActive) {
                gamepadBecameActive = true;
            }
            gamepadActive = true;
            return;
        }

        if (!gamepad) {
            gamepadActive = false;
            gamepadBecameActive = false;
            requestedFocus.reset();
        }
    }

    void NotifyGamepadAnalogInput() {
        if (!gamepadActive) {
            gamepadBecameActive = true;
        }
        gamepadActive = true;
    }

    bool IsActive() { return IsPanelNavigationActive(); }

    bool IsOptionsToggleRequested() { return IsPanelNavigationActive() && optionsToggleRequested; }

    void RequestFocus(Area area) {
        if (!IsPanelNavigationActive() || area == Area::Count) {
            return;
        }
        CancelDefaultMoveRequest();
        activeArea = area;
        requestedFocus = area;
    }

    void BeginArea(Area area) {
        const std::size_t areaIndex = AreaIndex(area);
        areaWindows[areaIndex] = ImGui::GetCurrentWindow();
        recordingArea = area;
        recordingItems = IsPanelNavigationActive() &&
                         (area == Area::PageTree || area == Area::PageContent || area == Area::OptionsMenu);

        if (!IsPanelNavigationActive()) {
            return;
        }

        ProcessAreaInput(area);
        ScrollArea(area);
    }

    void EndArea() {
        if (!recordingItems) {
            return;
        }

        recordingItems = false;
        const std::size_t areaIndex = AreaIndex(recordingArea);
        auto& items = currentItems[areaIndex];

        if (requestedFocus && *requestedFocus == recordingArea) {
            if (items.empty()) {
                FocusWindowWithoutItem(areaWindows[areaIndex]);
                focusedIndices[areaIndex] = 0;
            } else {
                focusedIndices[areaIndex] = std::min(focusedIndices[areaIndex], items.size() - 1);
                FocusItem(items[focusedIndices[areaIndex]]);
            }
            requestedFocus.reset();
        } else if (activeArea == recordingArea && !IsPopupBlockingAreaNavigation(recordingArea)) {
            const auto focusedItem = FindFocusedItem(items);
            if (focusedItem) {
                focusedIndices[areaIndex] = *focusedItem;
            } else if (!items.empty()) {
                focusedIndices[areaIndex] = std::min(focusedIndices[areaIndex], items.size() - 1);
                FocusItem(items[focusedIndices[areaIndex]]);
            }
        }

        previousItems[areaIndex] = items;
    }

    void BeginOptionsMenu() {
        optionsVisible = true;
        if (!IsPanelNavigationActive()) {
            return;
        }

        if (activeArea != Area::OptionsMenu) {
            areaBeforeOptions = activeArea;
            activeArea = Area::OptionsMenu;
            requestedFocus = Area::OptionsMenu;
            optionsNeedFocus = true;
        }

        if (optionsNeedFocus) {
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            FocusWindowWithoutItem(window);
            optionsNeedFocus = false;
        }

        BeginArea(Area::OptionsMenu);
    }

    void NotifyOptionsMenuClosed() {
        if (activeArea != Area::OptionsMenu) {
            return;
        }
        activeArea = areaBeforeOptions;
        requestedFocus = activeArea;
        optionsNeedFocus = false;
    }

    void NotifyPageClosed() {
        previousItems[AreaIndex(Area::PageContent)].clear();
        focusedIndices[AreaIndex(Area::PageContent)] = 0;
        if (gamepadActive) {
            activeArea = Area::PageTree;
            requestedFocus = Area::PageTree;
        }
    }

    BackAction ResolveBack(bool hasPage) {
        if (ImGui::IsAnyItemActive() ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            return BackAction::PassToImGui;
        }
        if (hasPage) {
            NotifyPageClosed();
            return BackAction::PoppedPage;
        }
        return BackAction::CloseMenu;
    }

    void PushFocusStyle() {
        if (!IsPanelNavigationActive() || focusStylePushed) {
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        focusStylePushed = true;
    }

    void PopFocusStyle() {
        if (!focusStylePushed) {
            return;
        }
        ImGui::PopStyleColor();
        focusStylePushed = false;
    }

    void RenderFocusedItemHighlight() {
        if (!IsPanelNavigationActive() || !panelWindow) {
            return;
        }

        ImGuiContext& context = *GImGui;
        ImGuiWindow* window = context.NavWindow;
        if (!window || context.NavId == 0 || !IsWindowInside(window, panelWindow)) {
            return;
        }

        ImRect rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[context.NavLayer]);
        rect.Expand(2.0f);
        rect.ClipWith(window->ClipRect);
        if (rect.IsInverted() || rect.GetWidth() <= 0.0f || rect.GetHeight() <= 0.0f) {
            return;
        }

        const bool active = context.ActiveId == context.NavId;
        ImVec4 fillColor = focusAccent;
        ImVec4 glowColor = focusAccent;
        ImVec4 outlineColor = focusAccent;
        fillColor.w *= active ? 0.30f : 0.16f;
        glowColor.w *= active ? 0.55f : 0.32f;
        outlineColor.w = std::max(outlineColor.w, active ? 0.95f : 0.78f);

        const float rounding = std::max(ImGui::GetStyle().FrameRounding, 3.0f);
        ImDrawList* drawList = ImGui::GetForegroundDrawList(window);
        drawList->PushClipRect(window->ClipRect.Min, window->ClipRect.Max, true);
        drawList->AddRectFilled(rect.Min, rect.Max, ImGui::ColorConvertFloat4ToU32(fillColor), rounding);

        ImRect glowRect = rect;
        glowRect.Expand(2.0f);
        drawList->AddRect(glowRect.Min, glowRect.Max, ImGui::ColorConvertFloat4ToU32(glowColor), rounding + 2.0f,
                          ImDrawFlags_RoundCornersAll, active ? 4.0f : 3.0f);
        drawList->AddRect(rect.Min, rect.Max, ImGui::ColorConvertFloat4ToU32(outlineColor), rounding,
                          ImDrawFlags_RoundCornersAll, active ? 3.0f : 2.0f);

        const float markerWidth = active ? 5.0f : 3.0f;
        drawList->AddRectFilled(rect.Min, ImVec2(rect.Min.x + markerWidth, rect.Max.y),
                                ImGui::ColorConvertFloat4ToU32(outlineColor), rounding, ImDrawFlags_RoundCornersLeft);
        drawList->PopClipRect();
    }

    float GetHintBarHeight() {
        if (!IsPanelNavigationActive()) {
            return 0.0f;
        }
        return ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y;
    }

    void RenderHintBar(bool hasPage) {
        if (!IsPanelNavigationActive()) {
            return;
        }

        const float height = GetHintBarHeight();
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild("##MCPGamepadHints", ImVec2(0.0f, height), ImGuiChildFlags_None, flags);
        ImGui::Separator();

        if (!GamepadIcons::IsAvailable()) {
            const char* warning = "ImGui Icons is required for controller prompts.";
            const float warningWidth = ImGui::CalcTextSize(warning).x;
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowSize().x - warningWidth) * 0.5f));
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", warning);
            ImGui::EndChild();
            return;
        }

        const std::vector<Hint> hints = GetHints(hasPage);
        const float iconSize = ImGui::GetTextLineHeight();
        const float itemSpacing = ImGui::GetStyle().ItemSpacing.x * 2.0f;
        float totalWidth = 0.0f;
        for (const Hint& hint : hints) {
            totalWidth += HintWidth(hint, iconSize);
        }
        if (hints.size() > 1) {
            totalWidth += itemSpacing * static_cast<float>(hints.size() - 1);
        }

        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowSize().x - totalWidth) * 0.5f));
        for (std::size_t index = 0; index < hints.size(); ++index) {
            RenderHint(hints[index], iconSize);
            if (index + 1 < hints.size()) {
                ImGui::SameLine(0.0f, itemSpacing);
            }
        }
        ImGui::EndChild();
    }
}
