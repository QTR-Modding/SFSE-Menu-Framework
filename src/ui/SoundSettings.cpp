#include "localization/Translations.h"
#include "ui/SoundSettings.h"
#include "audio/MenuSounds.h"
#include "config/FrameworkSettings.h"
#include "appearance/AssetDiscovery.h"

#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace SFSEMenuFramework::SoundSettings
{
    namespace
    {
        struct SoundFile { std::string Name; std::filesystem::path Path; };
        std::vector<SoundFile> files;
        bool discovered{};
        bool pendingSave{};

        void Discover()
        {
            Appearance::Detail::DiscoverFiles(
                L"Data/SFSE/Plugins/SFSEMenuFrameworkSounds", "sound", true, files,
                [](const std::filesystem::path& path, std::string& name) {
                    name = path.filename().string();
                    return FrameworkSettings::IsValidSoundFileName(name) &&
                        FrameworkSettings::EqualsIgnoreCaseAscii(path.extension().string(), ".wav");
                });
            discovered = true;
        }
    }

    void FinishEdit(bool& saveFailed)
    {
        if (pendingSave) {
            saveFailed = !FrameworkSettings::Save();
            pendingSave = false;
        }
    }

    void Render(bool& saveFailed)
    {
        // Finish a released volume edit before rendering the next frame.
        if (pendingSave && !ImGui::IsAnyItemActive()) {
            saveFailed = !FrameworkSettings::Save();
            pendingSave = false;
        }
        if (!discovered) Discover();
        auto snapshot = FrameworkSettings::CaptureSnapshot();
        auto& sounds = snapshot.Sounds;
        bool changed = ImGui::Checkbox(SFSEMenuFramework::Translations::Get("Enable menu sounds", "Enable menu sounds"), &sounds.Enabled);
        int volume = static_cast<int>(sounds.Volume * 100 + 0.5F);
        if (ImGui::SliderInt(SFSEMenuFramework::Translations::Get("Sound volume", "Sound volume"), &volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
            sounds.Volume = static_cast<float>(volume) / 100;
            changed = true;
        }
        ImGui::TextWrapped(SFSEMenuFramework::Translations::Get("Choose a sound for each event. Default uses a soft built-in tone.", "Choose a sound for each event. Default uses a soft built-in tone."));
        ImGui::TextWrapped(SFSEMenuFramework::Translations::Get("Custom files: Data/SFSE/Plugins/SFSEMenuFrameworkSounds. PCM16 WAV, mono/stereo, 8-192 kHz, up to 5 seconds and 4 MiB.", "Custom files: Data/SFSE/Plugins/SFSEMenuFrameworkSounds. PCM16 WAV, mono/stereo, 8-192 kHz, up to 5 seconds and 4 MiB."));
        if (ImGui::Button(SFSEMenuFramework::Translations::Get("Reload sounds", "Reload sounds"))) { Discover(); Audio::Reload(); }
        for (std::size_t i = 0; i < Audio::eventCount; ++i) {
            auto& binding = sounds.Bindings[i];
            ImGui::PushID(static_cast<int>(i));
            changed = ImGui::Checkbox(Translations::Get(Audio::descriptions[i].Label, Audio::descriptions[i].Label), &binding.Enabled) || changed;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7F);
            const char* preview = binding.File == Audio::defaultFile ? Translations::Get("Default", "Default") : binding.File.data();
            if (ImGui::BeginCombo("##File", preview)) {
                if (ImGui::Selectable(Translations::Get("Default", "Default"), binding.File == Audio::defaultFile)) {
                    binding.File = Audio::defaultFile;
                    changed = true;
                }
                for (const auto& file : files) {
                    if (ImGui::Selectable(file.Name.c_str(),
                        FrameworkSettings::EqualsIgnoreCaseAscii(binding.File.data(), file.Name))) {
                        binding.File.fill(0);
                        std::copy(file.Name.begin(), file.Name.end(), binding.File.begin());
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!sounds.Enabled || sounds.Volume <= 0);
            if (ImGui::Button(SFSEMenuFramework::Translations::Get("Preview", "Preview"))) {
                FrameworkSettings::RestoreSnapshot(snapshot);
                Audio::Preview(static_cast<Audio::Event>(i));
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (changed) {
            FrameworkSettings::RestoreSnapshot(snapshot);
            pendingSave = true;
        }
        if (pendingSave && !ImGui::IsAnyItemActive()) {
            saveFailed = !FrameworkSettings::Save();
            pendingSave = false;
        }
        if (saveFailed && ImGui::Button(SFSEMenuFramework::Translations::Get("Retry saving sounds", "Retry saving sounds"))) saveFailed = !FrameworkSettings::Save();
        const auto error = Audio::GetError();
        if (!error.empty()) ImGui::TextWrapped("%s", error.c_str());
    }
}
