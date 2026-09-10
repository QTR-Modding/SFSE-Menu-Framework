#pragma once

namespace SFSEMenuFramework::GamepadIcons
{
    enum class Slot
    {
        Up, Down, Left, Right, Confirm, Cancel, Options, RightShoulder, RightStick, Count
    };

    // Theme-colored geometry and text; no image files or texture ownership.
    void Draw(Slot slot, float size, bool playStation);
}
