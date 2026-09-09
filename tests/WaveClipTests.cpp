#include "audio/WaveClip.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

using namespace SFSEMenuFramework::Audio;
using Bytes = std::vector<std::uint8_t>;

namespace
{
    void Require(bool condition, std::string_view description)
    {
        if (!condition) {
            std::cerr << "FAIL: " << description << '\n';
            std::exit(1);
        }
    }

    void Put16(Bytes& bytes, std::uint16_t value)
    {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    void Set32(Bytes& bytes, std::size_t offset, std::uint32_t value)
    {
        for (std::size_t i = 0; i < 4; ++i) {
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    }

    void Put32(Bytes& bytes, std::uint32_t value)
    {
        const auto offset = bytes.size();
        bytes.resize(offset + 4);
        Set32(bytes, offset, value);
    }

    void Chunk(Bytes& bytes, std::string_view tag, const Bytes& payload)
    {
        bytes.insert(bytes.end(), tag.begin(), tag.end());
        Put32(bytes, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        if (payload.size() % 2 != 0) {
            bytes.push_back(0);
        }
    }

    Bytes Format(std::uint16_t channels = 1, std::uint32_t rate = 48000)
    {
        Bytes bytes;
        Put16(bytes, 1);
        Put16(bytes, channels);
        Put32(bytes, rate);
        Put32(bytes, rate * channels * 2);
        Put16(bytes, channels * 2);
        Put16(bytes, 16);
        return bytes;
    }

    Bytes Header()
    {
        return {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    }

    void Finish(Bytes& bytes)
    {
        Set32(bytes, 4, static_cast<std::uint32_t>(bytes.size() - 8));
    }

    Bytes Wave(const Bytes& format, const Bytes& samples)
    {
        auto bytes = Header();
        Chunk(bytes, "fmt ", format);
        Chunk(bytes, "data", samples);
        Finish(bytes);
        return bytes;
    }

    int Sample(const WaveClip& clip, std::size_t frame)
    {
        const int bits = clip.Samples[frame * 2] | (clip.Samples[frame * 2 + 1] << 8);
        return bits < 32768 ? bits : bits - 65536;
    }
}

int main()
{
    const Bytes samples{0, 0, 0xff, 0x7f, 0, 0x80};
    const auto valid = Wave(Format(), samples);
    const auto decoded = DecodeWave(valid);
    Require(decoded && decoded->Samples == samples && decoded->SampleRate == 48000 &&
        decoded->Channels == 1, "mono PCM round trip");
    Require(DecodeWave(Wave(Format(2, 192000), Bytes(8))).has_value(), "stereo high rate");
    Require(DecodeWave(Wave(Format(1, 8000), Bytes(80000))).has_value(), "five second limit");
    Require(!DecodeWave(Wave(Format(1, 8000), Bytes(80002))), "over duration limit");

    for (std::size_t size = 0; size < valid.size(); ++size) {
        auto truncated = Bytes(valid.begin(), valid.begin() + size);
        Require(!DecodeWave(truncated), "every truncated prefix fails");
        if (size >= 12) {
            Finish(truncated);
            Require(!DecodeWave(truncated), "internally truncated RIFF fails");
        }
    }
    auto unknown = Header();
    Chunk(unknown, "JUNK", Bytes{1, 2, 3});
    Chunk(unknown, "data", samples);
    Chunk(unknown, "fmt ", Format());
    Finish(unknown);
    Require(DecodeWave(unknown).has_value(), "odd unknown chunk and data before format");
    auto missingPad = valid;
    Chunk(missingPad, "JUNK", Bytes{1});
    missingPad.pop_back();
    Finish(missingPad);
    Require(!DecodeWave(missingPad), "missing odd chunk padding");

    for (const auto tag : {"fmt ", "data"}) {
        auto duplicate = valid;
        Chunk(duplicate, tag, tag == std::string_view("fmt ") ? Format() : samples);
        Finish(duplicate);
        Require(!DecodeWave(duplicate), "duplicate chunk");
    }
    auto malformed = valid;
    Set32(malformed, 16, 0xffffffffu);
    Require(!DecodeWave(malformed), "overflowing chunk size");
    malformed = valid;
    malformed.push_back(0);
    Require(!DecodeWave(malformed), "trailing bytes outside RIFF");
    Finish(malformed);
    Require(!DecodeWave(malformed), "incomplete chunk header");
    Require(!DecodeWave(Bytes(4 * 1024 * 1024 + 1)), "oversized input");
    Require(!DecodeWave(Wave(Format(), {})), "empty data");
    Require(!DecodeWave(Wave(Format(2), samples)), "partial stereo frame");
    for (const auto offset : {0u, 2u, 4u, 8u, 12u, 14u}) {
        auto format = Format();
        format[offset] ^= 0x40;
        // The sample-rate mutation is rejected because its byte rate no longer agrees.
        Require(!DecodeWave(Wave(format, samples)), "invalid format field");
    }
    Require(!DecodeWave(Wave(Format(1, 7999), samples)), "low sample rate");
    Require(!DecodeWave(Wave(Format(1, 192001), samples)), "high sample rate");
    auto extended = Format();
    Put16(extended, 0);
    Require(DecodeWave(Wave(extended, samples)).has_value(), "empty PCM extension");
    extended.back() = 1;
    Require(!DecodeWave(Wave(extended, samples)), "unsupported PCM extension");

    const auto tone = MakeTone(700, 1200, 0.1f);
    Require(tone.SampleRate == 48000 && tone.Channels == 1 && tone.Samples.size() == 9600,
        "tone format and duration");
    Require(Sample(tone, 0) == 0 && Sample(tone, 4799) == 0, "zero tone endpoints");
    int peak = 0;
    for (std::size_t frame = 0; frame < tone.Samples.size() / 2; ++frame) {
        peak = std::max(peak, std::abs(Sample(tone, frame)));
        if (frame < 10 || frame >= 4790) {
            Require(std::abs(Sample(tone, frame)) <= 2, "smooth fade endpoints");
        }
    }
    Require(peak > 2000 && peak <= 2400, "quiet nonzero tone amplitude");
    Require(MakeTone(20, 20000, 5).Samples.size() == 480000, "bounded maximum tone");
    for (const auto duration : {0.0f, -1.0f, 5.1f, 0.000001f,
             std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        Require(MakeTone(700, 1200, duration).Samples.empty(), "invalid tone duration");
    }
    Require(MakeTone(19, 1200, 0.1f).Samples.empty(), "low tone frequency");
    Require(MakeTone(700, 20001, 0.1f).Samples.empty(), "high tone frequency");
    Require(MakeTone(std::numeric_limits<float>::quiet_NaN(), 1200, 0.1f).Samples.empty(),
        "nonfinite tone frequency");
    std::cout << "WaveClip tests passed\n";
}
