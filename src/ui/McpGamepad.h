#pragma once

namespace SFSEMenuFramework::McpGamepad
{
    enum class Area { Tree, Content };

    // Called on the render thread, inside the main MCP window.
    void Begin(bool gamepad, bool hasPage);
    void RequestPageFocus();
    void BeginArea(Area area);
    float GetFooterHeight();
    void End();
}
