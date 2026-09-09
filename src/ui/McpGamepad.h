#pragma once


namespace SFSEMenuFramework::McpGamepad {
    enum class Area { PageTree, PageContent, OptionsMenu, Popup, Count };

    void BeginFrame(bool hasPage, bool suspended);
    void EndFrame();

    void NotifyInputDevice(bool gamepad);
    void NotifyGamepadAnalogInput();

    bool IsActive();
    bool IsOptionsToggleRequested();

    void RequestFocus(Area area);
    void BeginArea(Area area);
    void EndArea();
    void BeginOptionsMenu();
    void NotifyOptionsMenuClosed();
    void NotifyPageClosed();

    enum class BackAction { PassToImGui, PoppedPage, CloseMenu };
    BackAction ResolveBack(bool hasPage);

    void PushFocusStyle();
    void PopFocusStyle();
    void RenderFocusedItemHighlight();

    float GetHintBarHeight();
    void RenderHintBar(bool hasPage);
}
