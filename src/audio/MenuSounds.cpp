#include "audio/MenuSounds.h"
#include "audio/InteractionSounds.h"
#include "audio/WaveClip.h"
#include "config/FrameworkSettings.h"

#include <Windows.h>
#include <xaudio2.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace SFSEMenuFramework::Audio
{
    namespace
    {
        struct Request { Event Kind; bool Preview; };
        struct State
        {
            std::mutex Mutex;
            HANDLE Wake{};
            Settings Config;
            std::optional<Request> Pending;
            bool Reload{};
            std::string Error;
        };
        // Owned for the SFSE process lifetime; no audio teardown under the loader lock.
        State* state{};
        bool previewed{};
        bool startupFailed{};
        ULONGLONG lastSound{};

        void SetError(State& shared, std::string message)
        {
            std::scoped_lock lock{shared.Mutex};
            shared.Error = std::move(message);
        }

        std::optional<WaveClip> LoadClip(const Binding& binding, Event event)
        {
            if (binding.File == defaultFile) {
                constexpr std::array frequencies{620.0F, 440.0F, 700.0F, 800.0F, 540.0F, 400.0F, 1000.0F, 850.0F};
                const auto frequency = frequencies[static_cast<std::size_t>(event)];
                const bool quiet = event == Event::Hover || event == Event::Navigate;
                return MakeTone(frequency, frequency * (event == Event::Open || event == Event::ToggleOn ? 1.25F : 0.8F),
                    quiet ? 0.025F : 0.065F);
            }
            const auto directory = FrameworkSettings::BuildGamePath(L"Data/SFSE/Plugins/SFSEMenuFrameworkSounds");
            if (directory.empty()) return {};
            const auto file = directory / binding.File.data();
            std::error_code error;
            const auto length = std::filesystem::file_size(file, error);
            if (error || length < 44 || length > 4 * 1024 * 1024) return {};
            std::ifstream stream(file, std::ios::binary);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
            if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return {};
            return DecodeWave(bytes);
        }

        DWORD WINAPI Worker(void* argument)
        {
            auto& shared = *static_cast<State*>(argument);
            const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(com)) {
                SetError(shared, "Could not initialize Windows audio.");
                return 0;
            }
            IXAudio2* engine{};
            IXAudio2MasteringVoice* master{};
            IXAudio2SourceVoice* voice{};
            Settings loaded{};
            std::array<std::optional<WaveClip>, eventCount> clips{};
            std::array<bool, eventCount> attempted{};
            Event playing{Event::Hover};
            auto stopVoice = [&] {
                if (voice) { voice->DestroyVoice(); voice = nullptr; }
            };
            for (;;) {
                Settings config;
                std::optional<Request> request;
                bool reload;
                {
                    std::scoped_lock lock{shared.Mutex};
                    config = shared.Config;
                    request = std::exchange(shared.Pending, {});
                    reload = std::exchange(shared.Reload, false);
                }
                bool filesChanged = reload;
                for (std::size_t i = 0; i < eventCount; ++i)
                    filesChanged |= config.Bindings[i].File != loaded.Bindings[i].File;
                if (filesChanged || !config.Enabled) {
                    stopVoice(); // DestroyVoice finishes all reads before cached PCM is released.
                    clips = {};
                    attempted = {};
                }
                if (reload) {
                    if (master) { master->DestroyVoice(); master = nullptr; }
                    if (engine) { engine->Release(); engine = nullptr; }
                }
                loaded = config;
                if (!config.Enabled) {
                    if (master) { master->DestroyVoice(); master = nullptr; }
                    if (engine) { engine->Release(); engine = nullptr; }
                    WaitForSingleObject(shared.Wake, INFINITE);
                    continue;
                }
                if (voice) {
                    XAUDIO2_VOICE_STATE status{};
                    voice->GetState(&status, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                    if (!status.BuffersQueued) stopVoice();
                    else voice->SetVolume(config.Volume);
                }
                if (request) {
                    const auto index = static_cast<std::size_t>(request->Kind);
                    const bool allowed = request->Preview || config.Bindings[index].Enabled;
                    if (allowed && config.Volume > 0 &&
                        (!voice || request->Preview || Priority(request->Kind) >= Priority(playing))) {
                        if (!attempted[index]) {
                            clips[index] = LoadClip(config.Bindings[index], request->Kind);
                            attempted[index] = true;
                        }
                        if (!clips[index]) {
                            SetError(shared, "Could not load the selected sound. Use a PCM16 WAV, mono/stereo, at most 5 seconds and 4 MiB.");
                        } else {
                            {
                                std::scoped_lock lock{shared.Mutex};
                                // A file read may finish after the user disables sounds or changes the selection.
                                if (config != shared.Config) continue;
                            }
                            if (!engine && SUCCEEDED(XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
                                if (FAILED(engine->CreateMasteringVoice(&master))) {
                                    engine->Release();
                                    engine = nullptr;
                                }
                            }
                            if (!engine) {
                                SetError(shared, "Windows audio is unavailable. Check the output device and try again.");
                            } else {
                                stopVoice();
                                const auto& clip = *clips[index];
                                WAVEFORMATEX format{};
                                format.wFormatTag = WAVE_FORMAT_PCM;
                                format.nChannels = clip.Channels;
                                format.nSamplesPerSec = clip.SampleRate;
                                format.wBitsPerSample = 16;
                                format.nBlockAlign = static_cast<WORD>(clip.Channels * 2);
                                format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
                                XAUDIO2_BUFFER buffer{};
                                buffer.Flags = XAUDIO2_END_OF_STREAM;
                                buffer.AudioBytes = static_cast<UINT32>(clip.Samples.size());
                                buffer.pAudioData = clip.Samples.data();
                                if (SUCCEEDED(engine->StartEngine()) &&
                                    SUCCEEDED(engine->CreateSourceVoice(&voice, &format)) &&
                                    SUCCEEDED(voice->SetVolume(config.Volume)) &&
                                    SUCCEEDED(voice->SubmitSourceBuffer(&buffer)) &&
                                    SUCCEEDED(voice->Start())) {
                                    playing = request->Kind;
                                    SetError(shared, {});
                                } else {
                                    stopVoice();
                                    SetError(shared, "Could not play the selected sound. Try Reload sounds.");
                                }
                            }
                        }
                    }
                }
                if (!voice && engine) engine->StopEngine();
                WaitForSingleObject(shared.Wake, voice ? 20 : INFINITE);
            }
        }

        bool Configure(const Settings& config)
        {
            if (!state && config.Enabled) {
                auto* created = new State;
                created->Config = config;
                created->Wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                HANDLE thread = created->Wake ? CreateThread(nullptr, 0, Worker, created, 0, nullptr) : nullptr;
                if (!thread) {
                    if (created->Wake) CloseHandle(created->Wake);
                    delete created;
                    startupFailed = true;
                    return false;
                }
                CloseHandle(thread);
                state = created;
                startupFailed = false;
            }
            if (!state) return false;
            std::scoped_lock lock{state->Mutex};
            if (config != state->Config) {
                state->Config = config;
                if (!config.Enabled) state->Pending.reset();
                SetEvent(state->Wake);
            }
            return true;
        }

        void Queue(Event event, bool preview)
        {
            const auto config = FrameworkSettings::GetSoundSettings();
            if (!config.Enabled || !Configure(config)) return;
            std::scoped_lock lock{state->Mutex};
            state->Pending = Request{event, preview}; // Bounded: never build up a delayed sound queue.
            SetEvent(state->Wake);
        }
    }

    void BeginFrame()
    {
        previewed = false;
        const auto config = FrameworkSettings::GetSoundSettings();
        Configure(config);
        BeginInteractions(config.Enabled);
    }

    void EndFrame()
    {
        const auto event = EndInteractions();
        const auto config = FrameworkSettings::GetSoundSettings();
        Configure(config);
        if (!event || previewed || !config.Enabled ||
            !config.Bindings[static_cast<std::size_t>(*event)].Enabled) return;
        const auto now = GetTickCount64();
        const auto interval = *event == Event::Hover || *event == Event::Navigate ? 90ULL : 35ULL;
        if (now - lastSound < interval) return;
        lastSound = now;
        Queue(*event, false);
    }

    void Preview(Event event)
    {
        previewed = true;
        Queue(event, true);
    }

    void Reload()
    {
        if (!state) return;
        std::scoped_lock lock{state->Mutex};
        state->Reload = true;
        state->Error.clear();
        SetEvent(state->Wake);
    }

    std::string GetError()
    {
        if (!state) return startupFailed ? "Could not start the sound worker." : "";
        std::scoped_lock lock{state->Mutex};
        return state->Error;
    }
}
