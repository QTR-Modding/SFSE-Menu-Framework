#include "appearance/GamepadIcons.h"

#include <imgui.h>
#include <algorithm>

namespace SFSEMenuFramework::GamepadIcons
{
    void Draw(Slot slot, float size, bool playStation)
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 center(origin.x + size * 0.5f, origin.y + size * 0.5f);
        const float radius = size * 0.43f;
        const float stroke = std::max(1.0f, size * 0.055f);
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
        auto* draw = ImGui::GetWindowDrawList();
        const auto point = [&](float x, float y) {
            return ImVec2(center.x + x * radius, center.y + y * radius);
        };
        const auto label = [&](const char* text) {
            const float fontSize = size * 0.60f;
            auto* font = ImGui::GetFont();
            const ImVec2 extent = font->CalcTextSizeA(fontSize, size, 0.0f, text);
            draw->AddText(font, fontSize,
                ImVec2(center.x - extent.x * 0.5f, center.y - extent.y * 0.5f), color, text);
        };

        switch (slot) {
        case Slot::Up:
            draw->AddTriangleFilled(point(0, -0.8f), point(-0.7f, 0.5f), point(0.7f, 0.5f), color);
            break;
        case Slot::Down:
            draw->AddTriangleFilled(point(0, 0.8f), point(-0.7f, -0.5f), point(0.7f, -0.5f), color);
            break;
        case Slot::Left:
            draw->AddTriangleFilled(point(-0.8f, 0), point(0.5f, -0.7f), point(0.5f, 0.7f), color);
            break;
        case Slot::Right:
            draw->AddTriangleFilled(point(0.8f, 0), point(-0.5f, -0.7f), point(-0.5f, 0.7f), color);
            break;
        case Slot::RightShoulder:
            draw->AddRect(point(-1, -0.7f), point(1, 0.7f), color, size * 0.12f, 0, stroke);
            label(playStation ? "R1" : "RB");
            break;
        case Slot::RightStick:
            draw->AddCircle(center, radius, color, 0, stroke);
            label(playStation ? "R3" : "RS");
            break;
        case Slot::Confirm:
        case Slot::Cancel:
        case Slot::Options:
            draw->AddCircle(center, radius, color, 0, stroke);
            if (!playStation) {
                label(slot == Slot::Confirm ? "A" : slot == Slot::Cancel ? "B" : "X");
            } else if (slot == Slot::Confirm) {
                draw->AddLine(point(-0.45f, -0.45f), point(0.45f, 0.45f), color, stroke);
                draw->AddLine(point(-0.45f, 0.45f), point(0.45f, -0.45f), color, stroke);
            } else if (slot == Slot::Cancel) {
                draw->AddCircle(center, radius * 0.55f, color, 0, stroke);
            } else {
                draw->AddRect(point(-0.45f, -0.45f), point(0.45f, 0.45f), color, 0, 0, stroke);
            }
            break;
        case Slot::Count:
            break;
        }
        ImGui::Dummy(ImVec2(size, size));
    }
}
