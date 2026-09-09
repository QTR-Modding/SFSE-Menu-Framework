#include "WaveClip.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace SFSEMenuFramework::Audio
{
    namespace
    {
        constexpr std::size_t MaxFileBytes = 4 * 1024 * 1024;

        // Callers establish the complete field's bounds before reading it.
        std::uint16_t Read16(std::span<const std::uint8_t> bytes, std::size_t offset)
        {
            return static_cast<std::uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
        }

        std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t offset)
        {
            return static_cast<std::uint32_t>(bytes[offset]) |
                   (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                   (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                   (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        }

        bool Tag(std::span<const std::uint8_t> bytes, std::size_t offset, const char* tag)
        {
            return bytes[offset] == tag[0] && bytes[offset + 1] == tag[1] &&
                   bytes[offset + 2] == tag[2] && bytes[offset + 3] == tag[3];
        }
    }

    std::optional<WaveClip> DecodeWave(std::span<const std::uint8_t> bytes)
    {
        if (bytes.size() < 12 || bytes.size() > MaxFileBytes ||
            !Tag(bytes, 0, "RIFF") || !Tag(bytes, 8, "WAVE") ||
            Read32(bytes, 4) != bytes.size() - 8) {
            return std::nullopt;
        }

        WaveClip clip;
        bool hasFormat = false;
        bool hasData = false;
        std::span<const std::uint8_t> samples;
        std::size_t offset = 12;
        while (offset < bytes.size()) {
            if (bytes.size() - offset < 8) {
                return std::nullopt;
            }
            const auto size = static_cast<std::size_t>(Read32(bytes, offset + 4));
            const auto payload = offset + 8;
            if (size > bytes.size() - payload) {
                return std::nullopt;
            }
            const auto end = payload + size;
            const auto padding = size & 1;
            if (padding > bytes.size() - end) {
                return std::nullopt;
            }

            if (Tag(bytes, offset, "fmt ")) {
                // PCM's standard 16-byte format, or WAVEFORMATEX with no extension.
                if (hasFormat || (size != 16 && size != 18) ||
                    (size == 18 && Read16(bytes, payload + 16) != 0)) {
                    return std::nullopt;
                }
                hasFormat = true;
                const auto format = Read16(bytes, payload);
                clip.Channels = Read16(bytes, payload + 2);
                clip.SampleRate = Read32(bytes, payload + 4);
                const auto byteRate = Read32(bytes, payload + 8);
                const auto alignment = Read16(bytes, payload + 12);
                const auto bits = Read16(bytes, payload + 14);
                if (format != 1 || (clip.Channels != 1 && clip.Channels != 2) ||
                    clip.SampleRate < 8000 || clip.SampleRate > 192000 || bits != 16 ||
                    alignment != clip.Channels * 2 ||
                    byteRate != clip.SampleRate * alignment) {
                    return std::nullopt;
                }
            } else if (Tag(bytes, offset, "data")) {
                if (hasData) {
                    return std::nullopt;
                }
                hasData = true;
                samples = bytes.subspan(payload, size);
            }
            offset = end + padding;
        }

        const auto alignment = clip.Channels * 2u;
        if (!hasFormat || !hasData || samples.empty() || samples.size() % alignment != 0 ||
            samples.size() / alignment > clip.SampleRate * 5u) {
            return std::nullopt;
        }
        clip.Samples.assign(samples.begin(), samples.end());
        return clip;
    }

    WaveClip MakeTone(float startHz, float endHz, float seconds)
    {
        WaveClip clip;
        if (!std::isfinite(startHz) || !std::isfinite(endHz) || !std::isfinite(seconds) ||
            startHz < 20 || startHz > 20000 || endHz < 20 || endHz > 20000 ||
            seconds <= 0 || seconds > 5) {
            return clip;
        }
        const auto frames = static_cast<std::size_t>(static_cast<double>(seconds) * clip.SampleRate);
        if (frames < 2) {
            return clip;
        }
        clip.Samples.resize(frames * 2);
        const auto duration = static_cast<double>(frames - 1) / clip.SampleRate;
        const auto fade = std::min(0.015, duration / 2);
        for (std::size_t i = 0; i < frames; ++i) {
            const auto time = static_cast<double>(i) / clip.SampleRate;
            const auto fadePosition = std::clamp(std::min(time, duration - time) / fade, 0.0, 1.0);
            const auto envelope = 0.5 - 0.5 * std::cos(std::numbers::pi * fadePosition);
            const auto phase = 2 * std::numbers::pi *
                (startHz * time + (endHz - startHz) * time * time / (2 * duration));
            const auto sample = static_cast<std::int16_t>(std::lround(2400 * envelope * std::sin(phase)));
            const auto bits = static_cast<std::uint16_t>(sample);
            clip.Samples[i * 2] = static_cast<std::uint8_t>(bits & 0xff);
            clip.Samples[i * 2 + 1] = static_cast<std::uint8_t>(bits >> 8);
        }
        return clip;
    }
}
