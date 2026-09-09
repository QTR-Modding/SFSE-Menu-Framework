#include "ui/McpGamepad.h"
#include "appearance/GamepadIcons.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace Pad = SFSEMenuFramework::McpGamepad;

namespace
{
    void Check(bool condition, const char* message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }

    struct Fixture
    {
        ImGuiContext* Context = ImGui::CreateContext();
        ImGuiWindow* Tree{};
        ImGuiWindow* Content{};
        ImGuiWindow* Options{};
        ImGuiWindow* Popup{};
        ImGuiWindow* Nested{};
        std::array<ImGuiID, 3> TreeItems{};
        std::array<ImGuiID, 2> OptionItems{};
        std::vector<ImGuiID> ContentItems;
        int Activations{};
        unsigned int IconElements{};
        ImGuiID SameRowButton{};
        bool PlayStation{};
        float Value{};
        float Footer{};
        char Text[64]{};
        bool HasPage{};
        bool Suspended{};
        bool OpenPopup{};
        bool OptionsOpen{};
        bool CloseOptions{};
        bool OptionsRequested{};
        bool ShowNestedChild{};

        Fixture()
        {
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = {1100, 800};
            io.DeltaTime = 1.0F / 60.0F;
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
            io.Fonts->AddFontDefault();
            unsigned char* pixels{};
            int width{}, height{};
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            Pad::NotifyInputDevice(true);
            Frame();
            Frame();
        }

        ~Fixture() { ImGui::DestroyContext(Context); }

        void Key(ImGuiKey key, bool down)
        {
            ImGui::GetIO().AddKeyEvent(key, down);
            Frame();
        }

        void Tap(ImGuiKey key)
        {
            Key(key, true);
            Key(key, false);
        }

        void ExpectFocus(ImGuiWindow* window, ImGuiID id, const char* message) const
        {
            if (Context->NavWindow != window || Context->NavId != id) {
                std::cerr << "Focus: " << (Context->NavWindow ? Context->NavWindow->Name : "none")
                          << " / " << Context->NavId << "; expected "
                          << (window ? window->Name : "none") << " / " << id << '\n';
                Check(false, message);
            }
        }

        void RenderOptions()
        {
            if (!ImGui::BeginMenuBar()) return;
            const bool wasOpen = ImGui::IsPopupOpen("Options");
            OptionsRequested = Pad::IsOptionsToggleRequested();
            if (OptionsRequested && !wasOpen) ImGui::OpenPopup("Options");
            OptionsOpen = ImGui::BeginMenu("Options");
            if (OptionsOpen) {
                Options = ImGui::GetCurrentWindow();
                Pad::BeginOptionsMenu();
                if ((OptionsRequested && wasOpen) || CloseOptions) {
                    ImGui::CloseCurrentPopup();
                    Pad::NotifyOptionsMenuClosed();
                    CloseOptions = false;
                } else {
                    ImGui::MenuItem("First option");
                    OptionItems[0] = ImGui::GetItemID();
                    ImGui::MenuItem("Second option");
                    OptionItems[1] = ImGui::GetItemID();
                }
                Pad::EndArea();
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        void RenderTree(float height)
        {
            ImGui::BeginChild("Tree", {240, height}, ImGuiChildFlags_Border);
            Tree = ImGui::GetCurrentWindow();
            Pad::BeginArea(Pad::Area::PageTree);

            const bool clicked = ImGui::Button("Page");
            TreeItems[0] = ImGui::GetItemID();
            const bool activated = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) &&
                                   ImGui::IsItemFocused();
            if (clicked || activated) {
                HasPage = true;
                if (activated) Pad::RequestFocus(Pad::Area::PageContent);
            }
            ImGui::SameLine();
            ImGui::Button("Same row");
            TreeItems[1] = ImGui::GetItemID();

            ImGui::BeginDisabled();
            ImGui::Button("Disabled");
            ImGui::EndDisabled();
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            ImGui::Button("Not navigable");
            ImGui::PopItemFlag();
            ImGui::Button("Last tree item");
            TreeItems[2] = ImGui::GetItemID();

            Pad::EndArea();
            ImGui::EndChild();
        }

        void RenderContent(float height)
        {
            ImGui::BeginChild("Content", {0, height}, ImGuiChildFlags_Border);
            Content = ImGui::GetCurrentWindow();
            Pad::BeginArea(Pad::Area::PageContent);
            ContentItems.clear();
            if (HasPage) {
                if (ImGui::Button("First control")) ++Activations;
                ContentItems.push_back(ImGui::GetItemID());
                ImGui::SameLine();
                ImGui::Button("Load settings");
                SameRowButton = ImGui::GetItemID();
                ImGui::SliderFloat("Adjustment", &Value, 0.0F, 100.0F);
                ContentItems.push_back(ImGui::GetItemID());
                ImGui::InputText("Text", Text, sizeof(Text));
                ContentItems.push_back(ImGui::GetItemID());
                for (int i = 0; i < 32; ++i) {
                    ImGui::PushID(i);
                    ImGui::Button("Another control");
                    ContentItems.push_back(ImGui::GetItemID());
                    ImGui::PopID();
                }
            }
            if (ShowNestedChild) {
                ImGui::BeginChild("Nested", {100, 80}, ImGuiChildFlags_Border);
                Nested = ImGui::GetCurrentWindow();
                ImGui::Button("Nested control");
                ContentItems.push_back(ImGui::GetItemID());
                ImGui::EndChild();
            }
            if (OpenPopup) {
                ImGui::OpenPopup("Blocking popup");
                OpenPopup = false;
            }
            if (ImGui::BeginPopup("Blocking popup")) {
                Popup = ImGui::GetCurrentWindow();
                ImGui::Button("Popup control");
                ImGui::EndPopup();
            }
            Pad::EndArea();
            ImGui::EndChild();
        }

        void Frame()
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({20, 20});
            ImGui::SetNextWindowSize({900, 500});
            ImGui::Begin("MCP fixture", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_MenuBar);
            Pad::BeginFrame(HasPage, Suspended);
            const auto navColor = ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight);
            Pad::PushFocusStyle();
            RenderOptions();
            Footer = Pad::GetHintBarHeight();
            const float height = Footer > 0 ? -(Footer + ImGui::GetStyle().ItemSpacing.y) : -FLT_MIN;
            RenderTree(height);
            ImGui::SameLine();
            RenderContent(height);
            Pad::RenderHintBar(HasPage, PlayStation);
            Pad::RenderFocusedItemHighlight();
            Pad::PopFocusStyle();
            Check(ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight).w == navColor.w, "focus color is restored");
            Check(ImGui::GetCursorPosY() <= ImGui::GetWindowHeight(), "hint bar fits the window");
            Pad::EndFrame();
            ImGui::End();
            ImGui::Render();
            IconElements = 0;
            for (const auto* list : ImGui::GetDrawData()->CmdLists) {
                for (const auto& command : list->CmdBuffer) {
                    Check(command.GetTexID() == ImGui::GetIO().Fonts->TexID, "hints need only the font atlas");
                    IconElements += command.ElemCount;
                }
            }
        }
    };

    void TestSequentialNavigation(Fixture& ui)
    {
        ui.ExpectFocus(ui.Tree, ui.TreeItems[0], "initial focus enters the page tree");
        Check(ui.Footer > 0, "controller hints reserve space");
        ui.Tap(ImGuiKey_GamepadDpadDown);
        ui.ExpectFocus(ui.Tree, ui.TreeItems[1], "Down visits a same-line item before the next row");
        ui.Tap(ImGuiKey_GamepadDpadDown);
        ui.ExpectFocus(ui.Tree, ui.TreeItems[2], "disabled and NoNav items are excluded");
        ui.Tap(ImGuiKey_GamepadDpadDown);
        ui.ExpectFocus(ui.Tree, ui.TreeItems[0], "Down wraps to the first item");
        ui.Tap(ImGuiKey_GamepadDpadUp);
        ui.ExpectFocus(ui.Tree, ui.TreeItems[2], "Up wraps to the last item");
        ui.Tap(ImGuiKey_GamepadLStickDown);
        ui.ExpectFocus(ui.Tree, ui.TreeItems[0], "left stick uses sequential navigation");
    }

    void TestPageHandoffAndScroll(Fixture& ui)
    {
        ui.Key(ImGuiKey_GamepadFaceDown, true);
        Check(ui.HasPage, "A opens the selected page");
        ui.Frame();
        ui.Key(ImGuiKey_GamepadFaceDown, false);
        ui.ExpectFocus(ui.Content, ui.ContentItems[0], "A hands focus to the first page control");
        Check(ui.Activations == 0, "the opening A press does not also activate a page control");
        ui.Tap(ImGuiKey_GamepadFaceDown);
        Check(ui.Activations == 1, "a separate A press activates the focused control once");

        ui.Tap(ImGuiKey_GamepadDpadRight);
        ui.ExpectFocus(ui.Content, ui.SameRowButton, "Right selects the same-row button");
        ui.Tap(ImGuiKey_GamepadDpadLeft);
        ui.ExpectFocus(ui.Content, ui.ContentItems[0], "Left returns to the first button");
        ui.Tap(ImGuiKey_GamepadDpadDown);
        ui.ExpectFocus(ui.Content, ui.ContentItems[1], "Down skips the same-row button");
        ui.Tap(ImGuiKey_GamepadDpadUp);
        ui.ExpectFocus(ui.Content, ui.ContentItems[0], "Up returns to the control above");
        ui.Tap(ImGuiKey_GamepadLStickDown);
        ui.ExpectFocus(ui.Content, ui.ContentItems[1], "stick Down is spatial too");
        for (std::size_t i = 2; i < ui.ContentItems.size(); ++i) ui.Tap(ImGuiKey_GamepadDpadDown);
        ui.ExpectFocus(ui.Content, ui.ContentItems.back(), "spatial navigation reaches offscreen controls");
        Check(ui.Content->Scroll.y > 0, "offscreen focus scrolls into view");
        for (std::size_t i = 1; i < ui.ContentItems.size(); ++i) ui.Tap(ImGuiKey_GamepadDpadUp);
        ui.ExpectFocus(ui.Content, ui.ContentItems[0], "spatial navigation returns up the page");
        ui.Frame();
        const float before = ui.Content->Scroll.y;
        ui.Key(ImGuiKey_GamepadRStickDown, true);
        ui.Frame();
        ui.Frame();
        ui.Key(ImGuiKey_GamepadRStickDown, false);
        Check(ui.Content->Scroll.y > before, "right stick scrolls the focused area");
        ui.Tap(ImGuiKey_GamepadR1);
        ui.ExpectFocus(ui.Content, ui.ContentItems[0], "RB enters page controls; it does not toggle back");

        Pad::RequestFocus(Pad::Area::PageTree);
        ui.Frame();
        ui.ExpectFocus(ui.Tree, ui.TreeItems[0], "tree focus can be restored");
        ui.Tap(ImGuiKey_GamepadR1);
        ui.ExpectFocus(ui.Content, ui.ContentItems[0], "RB enters controls from the tree");
    }

    void TestNestedChild(Fixture& ui)
    {
        ui.ShowNestedChild = true;
        ui.Frame();
        ui.Frame();
        ImGui::SetScrollY(ui.Content, ui.Content->ScrollMax.y);
        ui.Frame();
        ui.Frame();
        ImGui::FocusWindow(ui.Nested);
        ImGui::SetFocusID(ui.ContentItems.back(), ui.Nested);
        ui.Frame();
        ui.ExpectFocus(ui.Nested, ui.ContentItems.back(), "nested child focus is preserved");
        Check(Pad::CanHandleBack(ui.Content->RootWindow), "Back recognizes nested page children");
        Check(Pad::ResolveBack(true) == Pad::BackAction::PoppedPage, "Back leaves nested page content");
        ui.Frame();
        ui.ExpectFocus(ui.Tree, ui.TreeItems[0], "Back restores the tree from a nested child");
        ui.Tap(ImGuiKey_GamepadR1);
        ui.ShowNestedChild = false;
        Pad::NotifyPageClosed();
        ui.Frame();
        ui.Tap(ImGuiKey_GamepadR1);
        ui.Frame();
    }

    void TestOptionsAndPopups(Fixture& ui)
    {
        ui.Key(ImGuiKey_GamepadFaceLeft, true);
        Check(ui.OptionsRequested, "X requests Options on its initial press");
        ui.Frame();
        Check(!ui.OptionsRequested && ui.OptionsOpen, "holding X does not repeatedly toggle Options");
        ui.Key(ImGuiKey_GamepadFaceLeft, false);
        ui.ExpectFocus(ui.Options, ui.OptionItems[0], "Options receives focus");
        ui.Tap(ImGuiKey_GamepadDpadDown);
        ui.ExpectFocus(ui.Options, ui.OptionItems[1], "Options items use sequential navigation");
        Check(Pad::ResolveBack(true) == Pad::BackAction::PassToImGui, "Back defers to an open Options popup");
        ui.Tap(ImGuiKey_GamepadFaceLeft);
        ui.Frame();
        Check(!ui.OptionsOpen, "a second X press closes Options");
        ui.ExpectFocus(ui.Content, ui.ContentItems[0], "closing Options restores the previous area");

        ui.OpenPopup = true;
        ui.Frame();
        ui.Frame();
        Check(Pad::ResolveBack(true) == Pad::BackAction::PassToImGui, "Back defers to a client popup");
        const auto popupId = ui.Context->NavId;
        ui.Tap(ImGuiKey_GamepadR1);
        ui.ExpectFocus(ui.Popup, popupId, "RB does not steal popup focus");
        ui.Tap(ImGuiKey_GamepadDpadDown);
        Check(ui.Context->NavWindow == ui.Popup, "area navigation does not steal popup focus");
        ui.Tap(ImGuiKey_GamepadFaceRight);
        ui.Frame();
        Check(ui.Context->OpenPopupStack.empty(), "B dismisses the client popup without leaving its page");
        ui.ExpectFocus(ui.Content, ui.ContentItems[0], "closing a popup restores content focus");
    }

    void TestDeviceAndBack(Fixture& ui)
    {
        Pad::NotifyInputDevice(false);
        ui.Frame();
        Check(!Pad::IsActive() && ui.Footer == 0, "mouse mode disables custom navigation and hints");
        Pad::NotifyGamepadAnalogInput();
        ui.Frame();
        ui.ExpectFocus(ui.Tree, ui.TreeItems[0], "gamepad reactivation focuses the tree");
        Check(Pad::IsActive(), "analog gamepad activity restores controller mode");

        ui.Suspended = true;
        ui.Frame();
        Check(!Pad::IsActive() && ui.Footer == 0, "settings suspension leaves custom navigation inactive");
        ui.Tap(ImGuiKey_GamepadFaceLeft);
        Check(!ui.OptionsOpen, "X does not open Options while suspended");
        ui.Suspended = false;
        ui.Frame();
        ui.ExpectFocus(ui.Tree, ui.TreeItems[0], "resuming restores tree focus");

        ui.Tap(ImGuiKey_GamepadR1);
        Check(ui.Content->RootWindowForNav != ui.Content->RootWindow, "fixture reproduces child navigation root");
        Check(Pad::CanHandleBack(ui.Content->RootWindow), "Back accepts the right-hand panel");
        Check(!Pad::CanHandleBack(ui.Tree), "Back does not mistake a sibling for the owning window");
        ui.Tap(ImGuiKey_GamepadDpadDown);
        ui.ExpectFocus(ui.Content, ui.ContentItems[1], "slider is reachable sequentially");
        ui.Tap(ImGuiKey_GamepadFaceDown);
        Check(ui.Context->ActiveId != 0, "A starts slider editing");
        const float oldValue = ui.Value;
        ui.Tap(ImGuiKey_GamepadDpadRight);
        Check(ui.Value > oldValue, "left/right adjusts the active slider");
        Check(Pad::ResolveBack(true) == Pad::BackAction::PassToImGui, "Back first exits active adjustment");
        ui.Tap(ImGuiKey_GamepadFaceRight);
        Check(ui.Context->ActiveId == 0, "B leaves the active slider");
        ui.Tap(ImGuiKey_GamepadDpadDown);
        ui.ExpectFocus(ui.Content, ui.ContentItems[2], "text input is reachable sequentially");
        ui.Tap(ImGuiKey_GamepadFaceUp);
        Check(ui.Context->ActiveId != 0, "Y starts text editing using ImGui's input binding");
        Check(Pad::ResolveBack(true) == Pad::BackAction::PassToImGui, "Back first exits active editing");
        ui.Tap(ImGuiKey_GamepadFaceRight);
        Check(ui.Context->ActiveId == 0, "B leaves the active editor");
        Check(Pad::ResolveBack(true) == Pad::BackAction::PoppedPage, "Back then leaves the selected page");
        ui.HasPage = false;
        ui.Frame();
        ui.ExpectFocus(ui.Tree, ui.TreeItems[0], "leaving a page restores tree focus");
        Check(Pad::ResolveBack(false) == Pad::BackAction::CloseMenu, "Back closes MCP when no page is selected");
    }

    void TestIconsAndWindowing(Fixture& ui)
    {
        ui.Frame();
        Check(ui.IconElements > 0, "controller hints render without image files");
        ui.PlayStation = true;
        ui.Frame();
        Check(ui.IconElements > 0, "PlayStation symbols render without image files");
        ui.HasPage = true;
        ui.Frame();
        Check(ui.IconElements > 0, "controller symbols render in every area");
        ui.Tap(ImGuiKey_GamepadR1);
        Check(ui.IconElements > 0, "controller symbols render in every area");
        ui.Key(ImGuiKey_GamepadFaceLeft, true);
        ui.Frame();
        Check(ui.IconElements > 0, "controller symbols render in every area");
        for (int frame = 0; frame < 20; ++frame) ui.Frame();
        Check(ui.Context->NavWindowingTarget != nullptr, "holding X retains ImGui window move/resize mode");
        ui.Key(ImGuiKey_GamepadFaceLeft, false);
        ui.Frame();
        Check(ui.Context->NavWindowingTarget == nullptr, "releasing X leaves windowing mode");
        ui.CloseOptions = true;
        ui.Frame();
        ui.Frame();
    }
}

int main()
{
    Fixture ui;
    TestSequentialNavigation(ui);
    TestPageHandoffAndScroll(ui);
    TestNestedChild(ui);
    TestOptionsAndPopups(ui);
    TestDeviceAndBack(ui);
    TestIconsAndWindowing(ui);
    std::cout << "MCP gamepad navigation tests passed\n";
}
