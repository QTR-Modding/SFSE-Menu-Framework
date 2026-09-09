#pragma once

// Optional notifications for the framework's own ImGui context.
// The callback is set only during its render pass; no public ImGui ABI changes.
namespace ImGuiAudio
{
    enum class Action { Activate, ToggleOn, ToggleOff };
    inline void (*Callback)(Action){};
    inline void Notify(Action action)
    {
        if (Callback) Callback(action);
    }
}
