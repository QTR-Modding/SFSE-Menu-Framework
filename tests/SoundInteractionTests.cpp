#include "audio/InteractionSounds.h"
#include "config/FrameworkSettings.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace A = SFSEMenuFramework::Audio;
namespace F = SFSEMenuFramework::FrameworkSettings;
void Check(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main()
{
    auto* context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800, 600};
    io.DeltaTime = 1.0F / 60;
    io.Fonts->AddFontDefault();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    bool checked{}, checkbox{}, disabled{};
    ImVec2 target{};
    ImGuiID targetId{};
    auto frame = [&](bool enabled = true, bool visible = true) {
        ImGui::NewFrame();
        A::BeginInteractions(enabled);
        if (visible) {
            ImGui::SetNextWindowPos({20, 20});
            ImGui::SetNextWindowSize({400, 250});
            ImGui::Begin("Sound test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
            ImGui::BeginDisabled(disabled);
            if (checkbox) ImGui::Checkbox("Control", &checked);
            else ImGui::Button("Control", {140, 30});
            const auto minimum = ImGui::GetItemRectMin();
            const auto maximum = ImGui::GetItemRectMax();
            target = {(minimum.x + maximum.x) * 0.5F, (minimum.y + maximum.y) * 0.5F};
            targetId = ImGui::GetItemID();
            ImGui::EndDisabled();
            ImGui::End();
        }
        auto result = A::EndInteractions();
        ImGui::Render();
        return result;
    };
    Check(frame() == A::Event::Open, "window open");
    io.AddMousePosEvent(target.x, target.y);
    Check(frame() == A::Event::Hover, "hover entry");
    Check(!frame(), "stationary hover stays silent");
    io.AddMouseButtonEvent(0, true);
    Check(!frame(), "mouse down alone is not confirmation");
    io.AddMouseButtonEvent(0, false);
    Check(frame() == A::Event::Activate, "mouse release confirms");
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMousePosEvent(600, 500);
    io.AddMouseButtonEvent(0, false);
    Check(frame() != A::Event::Activate, "cancelled click stays silent");

    checkbox = true;
    frame();
    io.AddMousePosEvent(target.x, target.y);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    Check(frame() == A::Event::ToggleOn && checked, "checkbox emits one toggle, not click");
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    Check(frame() == A::Event::ToggleOff && !checked, "toggle off");
    disabled = true;
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMouseButtonEvent(0, false);
    Check(!frame(), "disabled control silent");
    disabled = false;
    checkbox = false;
    io.AddMousePosEvent(600, 500);
    frame();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    context->NavWindow = ImGui::FindWindowByName("Sound test");
    context->NavId = targetId;
    context->NavDisableHighlight = false;
    context->NavDisableMouseHover = true;
    frame();
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
    Check(frame() == A::Event::Activate, "gamepad confirms");
    Check(!frame(), "holding gamepad confirm does not repeat");
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
    frame();
    Check(!frame(false), "master disable");
    Check(frame(true, false) == A::Event::Close, "window close");
    ImGui::DestroyContext();

    const auto ini = F::BuildGamePath(L"Data/SFSE/Plugins/SFSEMenuFramework.ini");
    Check(!std::filesystem::exists(ini), "refusing to overwrite an existing settings file");
    std::filesystem::create_directories(ini.parent_path());
    auto saved = F::CaptureSnapshot();
    saved.Sounds.Enabled = true;
    saved.Sounds.Volume = 0.6F;
    auto& sound = saved.Sounds.Bindings[static_cast<std::size_t>(A::Event::Activate)];
    sound.File = {'c', 'l', 'i', 'c', 'k', '.', 'w', 'a', 'v'};
    sound.Enabled = false;
    F::RestoreSnapshot(saved);
    Check(F::Save(), "save audio choices");
    F::ResetDefaults();
    Check(F::Load(), "load audio choices");
    Check(F::GetSoundSettings() == saved.Sounds, "audio settings round trip");
    Check(!F::IsValidSoundFileName("../escape.wav"), "reject path traversal");
    Check(!F::IsValidSoundFileName("sound.mp3"), "reject unsupported filename");
    Check(F::IsValidSoundFileName("click.WAV"), "case insensitive extension");
    std::filesystem::remove(ini);
    std::cout << "Sound interaction and settings tests passed\n";
}
