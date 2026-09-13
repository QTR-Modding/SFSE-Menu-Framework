#include "ui/McpGamepad.h"
#include "localization/Translations.h"
#include "appearance/GamepadIcons.h"
#include "ItemObserver.h"

// Ported from SKSE Menu Framework 3, GamepadNavigation.cpp at
// 8fb2d295aee582a24204b015f39214ad43717728 (GPL-3.0-only).

#include <algorithm>
#include <array>
#include <cfloat>
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
        bool collectingWindows = false;
        int stickAxis = -1;
        std::vector<NavigableItem> windowItems;
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

        ImGuiWindow* NavigationScope(ImGuiWindow* window) {
            while ((window->Flags & ImGuiWindowFlags_ChildWindow) && window->ParentWindow)
                window = window->ParentWindow;
            return window;
        }

        bool IsChildContainer(const ImGuiContext* context, const ImGuiWindow* parent, ImGuiID id) {
            for (ImGuiWindow* candidate : context->Windows) {
                if (candidate->LastFrameActive == context->FrameCount &&
                    candidate->ParentWindow == parent && candidate->ChildId == id) {
                    return true;
                }
            }
            return false;
        }

        void ObserveItem(ImGuiContext* context, ImGuiWindow* window, const ImGuiLastItemData* itemData) {
            if (!gamepadActive || (!recordingItems && !collectingWindows) ||
                !context || !window || !itemData || itemData->ID == 0) {
                return;
            }
            if (itemData->InFlags & (ImGuiItemFlags_NoNav | ImGuiItemFlags_Disabled)) {
                return;
            }
            if (window->Flags & ImGuiWindowFlags_Tooltip) {
                return;
            }
            if (itemData->NavRect.GetWidth() <= 0.0f || itemData->NavRect.GetHeight() <= 0.0f) {
                return;
            }
            if (IsChildContainer(context, window, itemData->ID)) {
                return;
            }

            const bool areaItem = recordingItems && (!(NavigationScope(window)->Flags & ImGuiWindowFlags_Popup) ||
                (recordingArea == Area::OptionsMenu && window == areaWindows[AreaIndex(Area::OptionsMenu)]));
            if (!areaItem && !collectingWindows) return;
            auto& items = areaItem ? currentItems[AreaIndex(recordingArea)] : windowItems;
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

        constexpr std::array dpad{ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
            ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown};
        constexpr std::array stick{ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
            ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown};

        ImGuiDir StickDirection() {
            std::array<float, 4> strength{};
            for (std::size_t i = 0; i < stick.size(); ++i) {
                const auto* key = ImGui::GetKeyData(stick[i]);
                strength[i] = key->Down ? key->AnalogValue : 0.0f;
            }
            const float horizontal = std::max(strength[0], strength[1]);
            const float vertical = std::max(strength[2], strength[3]);
            if (horizontal == 0.0f && vertical == 0.0f) {
                stickAxis = -1;
                return ImGuiDir_None;
            }
            // Keep the stronger axis; a small wobble near a diagonal must not
            // switch it. The existing activation threshold and repeat rate stay intact.
            if (stickAxis < 0) stickAxis = vertical > horizontal ? 1 : 0;
            else if (stickAxis == 0 && vertical > horizontal * 1.25f) stickAxis = 1;
            else if (stickAxis == 1 && horizontal > vertical * 1.25f) stickAxis = 0;
            const int direction = stickAxis == 0 ?
                (strength[0] > strength[1] ? 0 : 1) : (strength[2] > strength[3] ? 2 : 3);
            return static_cast<ImGuiDir>(direction);
        }

        ImGuiDir ReadDirection() {
            for (std::size_t i = 0; i < dpad.size(); ++i)
                if (ImGui::IsKeyPressed(dpad[i], true)) return static_cast<ImGuiDir>(i);
            const ImGuiDir direction = StickDirection();
            return direction != ImGuiDir_None && ImGui::IsKeyPressed(stick[direction], true) ?
                direction : ImGuiDir_None;
        }

        float StickTweak(ImGuiAxis axis) {
            if (!gamepadActive || GImGui->NavInputSource != ImGuiInputSource_Gamepad) return 0.0f;
            // A physical D-pad press takes precedence; never double the adjustment.
            for (const auto key : dpad)
                if (ImGui::IsKeyDown(key)) return 0.0f;
            const ImGuiDir direction = StickDirection();
            if (direction == ImGuiDir_None ||
                (axis == ImGuiAxis_X) != (direction == ImGuiDir_Left || direction == ImGuiDir_Right))
                return 0.0f;
            float delay{}, rate{};
            ImGui::GetTypematicRepeatRate(ImGuiInputFlags_RepeatRateNavTweak, &delay, &rate);
            const float amount = static_cast<float>(ImGui::GetKeyPressedAmount(stick[direction], delay, rate));
            return direction == ImGuiDir_Left || direction == ImGuiDir_Up ? -amount : amount;
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
            return navWindow && (NavigationScope(navWindow)->Flags & ImGuiWindowFlags_Popup) &&
                   NavigationScope(navWindow) != optionsWindow;
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
            const auto sameRow = [&](std::size_t a, std::size_t b) {
                return items[a].Rect.Min.y < items[b].Rect.Max.y &&
                       items[b].Rect.Min.y < items[a].Rect.Max.y;
            };
            const std::size_t origin = index;
            do {
                index = direction < 0 ? (index == 0 ? items.size() - 1 : index - 1) :
                    (index + 1) % items.size();
            } while (area == Area::PageTree && index != origin && sameRow(origin, index));
            // Vertical tree movement lands on the row label, not its actions.
            if (area == Area::PageTree)
                while (index > 0 && sameRow(index - 1, index)) --index;

            focusedIndices[areaIndex] = index;
            FocusItem(items[index]);
        }

        void MoveSpatially(const std::vector<NavigableItem>& items, ImGuiDir direction, bool sameRowOnly = false) {
            const auto focused = FindFocusedItem(items);
            if (!focused) return;
            const ImRect& origin = items[*focused].Rect;
            const bool vertical = direction == ImGuiDir_Up || direction == ImGuiDir_Down;
            const bool forward = direction == ImGuiDir_Right || direction == ImGuiDir_Down;
            const auto low = [vertical](const ImRect& r) { return vertical ? r.Min.y : r.Min.x; };
            const auto high = [vertical](const ImRect& r) { return vertical ? r.Max.y : r.Max.x; };
            const auto crossLow = [vertical](const ImRect& r) { return vertical ? r.Min.x : r.Min.y; };
            const auto crossHigh = [vertical](const ImRect& r) { return vertical ? r.Max.x : r.Max.y; };
            std::optional<std::size_t> best;
            float bestScore = FLT_MAX;
            for (std::size_t index = 0; index < items.size(); ++index) {
                if (index == *focused || items[index].Layer != items[*focused].Layer) continue;
                const ImRect& candidate = items[index].Rect;
                if (sameRowOnly && (candidate.Min.y >= origin.Max.y || candidate.Max.y <= origin.Min.y)) continue;
                const float gap = forward ? low(candidate) - high(origin) : low(origin) - high(candidate);
                if (gap < -0.5f) continue;
                const float crossGap = std::max({0.0f,
                    crossLow(candidate) - crossHigh(origin), crossLow(origin) - crossHigh(candidate)});
                const float score = gap * gap + crossGap * crossGap * 4.0f;
                if (score < bestScore) {
                    bestScore = score;
                    best = index;
                }
            }
            if (best) {
                FocusItem(items[*best]);
            }
        }

        void ProcessAreaInput(Area area) {
            if (activeArea != area || IsPopupBlockingAreaNavigation(area) ||
                ImGui::IsAnyItemActive() || GImGui->NavWindowingTarget) {
                return;
            }

            const ImGuiDir direction = ReadDirection();
            if (area == Area::PageContent) {
                if (direction != ImGuiDir_None || GImGui->NavInputSource == ImGuiInputSource_Gamepad)
                    CancelDefaultMoveRequest();
                if (direction != ImGuiDir_None) MoveSpatially(previousItems[AreaIndex(area)], direction);
            } else if (direction == ImGuiDir_Up || direction == ImGuiDir_Down) {
                CancelDefaultMoveRequest();
                MoveSequentially(area, direction == ImGuiDir_Up ? -1 : 1);
            } else if (area == Area::PageTree && direction != ImGuiDir_None) {
                CancelDefaultMoveRequest();
                MoveSpatially(previousItems[AreaIndex(area)], direction, true);
            }
        }

        void ScrollFocusedWindow(ImGuiWindow* boundary, float direction) {
            if (!boundary || direction == 0.0f) return;
            ImGuiWindow* window = GImGui->NavWindow;
            if (!window || !IsWindowInside(window, boundary)) window = boundary;
            while (window->ScrollMax.y <= 0.0f && window != boundary) {
                ImGuiWindow* parent = window->ParentWindow;
                if (!parent || !IsWindowInside(parent, boundary)) return;
                window = parent;
            }
            if (window->ScrollMax.y <= 0.0f) return;
            const float deltaTime = ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : 1.0f / 60.0f;
            ImGui::SetScrollY(window, window->Scroll.y + direction * kScrollSpeed * deltaTime);
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

            ScrollFocusedWindow(areaWindows[AreaIndex(area)], direction);
        }

        std::vector<Hint> GetHints(bool hasPage) {
            constexpr IconSlot noSecondaryIcon = IconSlot::Count;
            switch (IsPopupBlockingAreaNavigation() ? Area::Popup : activeArea) {
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
                        {IconSlot::Left, IconSlot::Right, "Navigate / adjust"},
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
            width += ImGui::CalcTextSize(Translations::Get(hint.Text, hint.Text)).x;
            return width;
        }

        void RenderHint(const Hint& hint, float iconSize, bool playStation) {
            ImGui::BeginGroup();
            GamepadIcons::Draw(hint.PrimaryIcon, iconSize, playStation);
            if (hint.SecondaryIcon != IconSlot::Count) {
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                GamepadIcons::Draw(hint.SecondaryIcon, iconSize, playStation);
            }
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Translations::Get(hint.Text, hint.Text));
            ImGui::EndGroup();
        }
        void RenderHighlight(ImGuiWindow* boundary, ImVec4 accent) {
            if (!gamepadActive || !boundary) {
                return;
            }

            ImGuiContext& context = *GImGui;
            ImGuiWindow* window = context.NavWindow;
            if (!window || context.NavId == 0 || !IsWindowInside(window, boundary)) {
                return;
            }

            ImRect rect = ImGui::WindowRectRelToAbs(window, window->NavRectRel[context.NavLayer]);
            rect.Expand(2.0f);
            rect.ClipWith(window->ClipRect);
            if (rect.IsInverted() || rect.GetWidth() <= 0.0f || rect.GetHeight() <= 0.0f) {
                return;
            }

            const bool active = context.ActiveId == context.NavId;
            ImVec4 fillColor = accent;
            ImVec4 glowColor = accent;
            ImVec4 outlineColor = accent;
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

    }

    void BeginWindows(bool useGamepad) {
        NotifyInputDevice(useGamepad);
        windowItems.clear();
        collectingWindows = true;
        ImGui::SetItemAddObserver(ObserveItem);
        ImGui::SetNavTweakProvider(StickTweak);
    }

    void EndWindows() {
        collectingWindows = false;
        ImGui::SetItemAddObserver(nullptr);
        ImGui::SetNavTweakProvider(nullptr);
        auto& context = *GImGui;
        if (!gamepadActive || !context.NavWindow || context.NavWindowingTarget) return;
        // Child panels share their owning window; a popup remains its own scope.
        ImGuiWindow* scope = NavigationScope(context.NavWindow);
        // Main-panel areas already process their own movement and scrolling.
        if (scope == panelWindow && lastRenderedFrame == context.FrameCount && !suspended) return;
        std::vector<NavigableItem> eligible;
        for (const auto& item : windowItems) {
            ImGuiWindow* owner = NavigationScope(item.Window);
            if (owner == scope) eligible.push_back(item);
        }
        if (!ImGui::IsAnyItemActive()) {
            const ImGuiDir direction = ReadDirection();
            if (FindFocusedItem(eligible)) {
                if (direction != ImGuiDir_None || context.NavInputSource == ImGuiInputSource_Gamepad)
                    CancelDefaultMoveRequest();
                if (direction != ImGuiDir_None) MoveSpatially(eligible, direction);
            }
            const float scroll = (ImGui::IsKeyDown(ImGuiKey_GamepadRStickDown) ? 1.0f : 0.0f) -
                                 (ImGui::IsKeyDown(ImGuiKey_GamepadRStickUp) ? 1.0f : 0.0f);
            ScrollFocusedWindow(scope, scroll);
        }
        RenderHighlight(scope, ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight));
    }

    void BeginFrame(bool hasPage, bool shouldSuspend) {
        ImGui::SetItemAddObserver(ObserveItem);

        panelWindow = ImGui::GetCurrentWindow();
        focusAccent = ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight);
        const auto* focusedWindow = GImGui->NavWindow;
        const bool validFocus = focusedWindow && focusedWindow->LastFrameActive >= ImGui::GetFrameCount() - 1;
        suspended = shouldSuspend && validFocus && !IsWindowInside(focusedWindow, panelWindow);
        const bool optionsBlocked = IsPopupBlockingAreaNavigation() &&
            (activeArea != Area::OptionsMenu || IsPopupBlockingAreaNavigation(Area::OptionsMenu));
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

        if (!validFocus || firstFrameAfterOpen || gamepadBecameActive || (wasSuspended && !suspended)) {
            const bool outsideFocus = validFocus &&
                (!IsWindowInside(focusedWindow, panelWindow) || IsPopupBlockingAreaNavigation());
            if (!outsideFocus) {
                activeArea = Area::PageTree;
                requestedFocus = Area::PageTree;
            }
            gamepadBecameActive = false;
        }
        wasSuspended = suspended;

        optionsToggleRequested = !optionsBlocked &&
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) && !ImGui::IsAnyItemActive();

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

        if (!collectingWindows) ImGui::SetItemAddObserver(nullptr);
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
        } else if (activeArea == recordingArea && IsWindowInside(GImGui->NavWindow, panelWindow) &&
                   !IsPopupBlockingAreaNavigation(recordingArea)) {
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

    bool CanHandleBack(const ImGuiWindow* mainWindow) {
        const auto* context = ImGui::GetCurrentContext();
        return mainWindow && context && context->NavWindow && !context->NavWindowingTarget &&
               IsWindowInside(context->NavWindow, mainWindow);
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
        if (IsPanelNavigationActive()) RenderHighlight(panelWindow, focusAccent);
    }

    float GetHintBarHeight() {
        if (!IsPanelNavigationActive()) {
            return 0.0f;
        }
        return ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y;
    }

    void RenderHintBar(bool hasPage, bool playStation) {
        if (!IsPanelNavigationActive()) {
            return;
        }

        const float height = GetHintBarHeight();
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild("##MCPGamepadHints", ImVec2(0.0f, height), ImGuiChildFlags_None, flags);
        ImGui::Separator();

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
            RenderHint(hints[index], iconSize, playStation);
            if (index + 1 < hints.size()) {
                ImGui::SameLine(0.0f, itemSpacing);
            }
        }
        ImGui::EndChild();
    }
}
