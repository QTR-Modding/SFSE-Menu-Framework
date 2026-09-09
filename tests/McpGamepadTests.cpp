#include "ui/McpGamepad.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdlib>
#include <iostream>

namespace Pad = SFSEMenuFramework::McpGamepad;
void Check(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main()
{
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1000, 800};
    io.DeltaTime = 1.0F / 60;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGuiWindow* tree{};
    ImGuiWindow* content{};
    ImGuiID contentButton{};
    int activations{};
    bool gamepad = true, page = true, requestPage{}, seedTree{}, popup{};
    bool otherWindow{};
    ImGuiWindow* other{};
    float footer{};
    auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({650, 400});
        ImGui::Begin("MCP fixture", nullptr, ImGuiWindowFlags_NoDecoration);
        Pad::Begin(gamepad, page);
        footer = Pad::GetFooterHeight();
        const float panelHeight = footer > 0 ? -(footer + ImGui::GetStyle().ItemSpacing.y) : -FLT_MIN;
        ImGui::BeginChild("Tree", {190, panelHeight}, ImGuiChildFlags_Border);
        tree = ImGui::GetCurrentWindow();
        if (seedTree) {
            ImGui::FocusWindow(tree);
            ImGui::NavInitWindow(tree, true);
            seedTree = false;
        }
        Pad::BeginArea(Pad::Area::Tree);
        ImGui::Button("Page");
        ImGui::SameLine();
        ImGui::Button("Favorite");
        if (requestPage) { Pad::RequestPageFocus(); requestPage = false; }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("Content", {0, panelHeight}, ImGuiChildFlags_Border);
        content = ImGui::GetCurrentWindow();
        Pad::BeginArea(Pad::Area::Content);
        if (page) {
            if (ImGui::Button("First setting")) ++activations;
            contentButton = ImGui::GetItemID();
            for (int i = 0; i < 30; ++i) {
                ImGui::PushID(i);
                ImGui::Button("More");
                ImGui::PopID();
            }
        }
        if (popup) ImGui::OpenPopup("Popup");
        if (ImGui::BeginPopup("Popup")) {
            ImGui::Button("Popup control");
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        Pad::End();
        Check(ImGui::GetCursorPosY() <= ImGui::GetWindowHeight(), "footer fits the main window");
        ImGui::End();
        if (otherWindow) {
            ImGui::Begin("Other window");
            other = ImGui::GetCurrentWindow();
            ImGui::Button("Other control");
            ImGui::End();
        }
        ImGui::Render();
    };
    seedTree = true;
    frame(); frame();
    Check(context->NavWindow == tree, "fixture begins in tree");
    Check(footer > 0, "controller hints reserve space");
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
    requestPage = true;
    frame(); frame();
    Check(context->NavWindow == tree, "held A does not hand off yet");
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
    frame(); frame();
    Check(context->NavWindow == content && context->NavId == contentButton, "release focuses the first page control");
    Check(activations == 0, "opening a page does not activate its first control");

    io.AddKeyEvent(ImGuiKey_GamepadR1, true);
    frame(); frame();
    Check(context->NavWindow == tree, "RB returns to tree");
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    frame();
    io.AddKeyEvent(ImGuiKey_GamepadR1, true);
    frame(); frame();
    Check(context->NavWindow == content, "RB returns to page");
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    frame();

    popup = true;
    frame();
    popup = false;
    auto* popupWindow = context->NavWindow;
    io.AddKeyEvent(ImGuiKey_GamepadR1, true);
    frame();
    Check(context->NavWindow == popupWindow, "RB does not steal popup focus");
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
    frame();
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
    frame();

    gamepad = false;
    requestPage = true;
    frame();
    Check(footer == 0, "mouse mode has no controller footer");
    gamepad = true;
    page = false;
    frame(); frame();
    Check(context->NavWindow == tree, "removed page returns focus to tree");

    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, true);
    io.AddKeyEvent(ImGuiKey_GamepadR1, true);
    frame();
    Check(context->NavWindow == tree, "window controls retain RB while X is held");
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, false);
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    frame();
    page = true;
    otherWindow = true;
    frame(); frame();
    Check(context->NavWindow == other, "other window owns focus");
    io.AddKeyEvent(ImGuiKey_GamepadR1, true);
    frame();
    Check(context->NavWindow == other, "RB does not steal another window's focus");
    io.AddKeyEvent(ImGuiKey_GamepadR1, false);
    ImGui::DestroyContext();
    std::cout << "MCP gamepad navigation tests passed\n";
}
