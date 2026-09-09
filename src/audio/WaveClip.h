#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace SFSEMenuFramework::Audio
{
    struct WaveClip
    {
        std::uint32_t SampleRate = 48000;
        std::uint16_t Channels = 1;
        std::vector<std::uint8_t> Samples;
    };

    // Accepts complete RIFF/WAVE PCM16 files, up to 4 MiB and five seconds.
    [[nodiscard]] std::optional<WaveClip> DecodeWave(std::span<const std::uint8_t> bytes);

    // Invalid parameters produce an empty clip. Output is mono, 48 kHz PCM16.
    [[nodiscard]] WaveClip MakeTone(float startHz, float endHz, float seconds);
}
