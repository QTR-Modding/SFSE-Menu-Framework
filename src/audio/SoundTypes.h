#pragma once

#include <array>
#include <cstddef>

namespace SFSEMenuFramework::Audio
{
    enum class Event : unsigned char { Open, Close, Activate, ToggleOn, ToggleOff, Back, Hover, Navigate, Count };
    inline constexpr auto eventCount = static_cast<std::size_t>(Event::Count);
    struct EventDescription { const char* Label; const wchar_t* Section; };
    inline constexpr std::array descriptions{
        EventDescription{"Open", L"Sound.Open"}, EventDescription{"Close", L"Sound.Close"},
        EventDescription{"Click / confirm", L"Sound.Activate"}, EventDescription{"Toggle on", L"Sound.ToggleOn"},
        EventDescription{"Toggle off", L"Sound.ToggleOff"}, EventDescription{"Back / cancel", L"Sound.Back"},
        EventDescription{"Mouse hover", L"Sound.Hover"}, EventDescription{"Keyboard / gamepad navigation", L"Sound.Navigate"}
    };
    inline constexpr std::array<char, 64> defaultFile{'D', 'E', 'F', 'A', 'U', 'L', 'T'};
    struct Binding
    {
        bool Enabled{true};
        std::array<char, 64> File{defaultFile};
        bool operator==(const Binding&) const = default;
    };
    struct Settings
    {
        bool Enabled{};
        float Volume{0.35F};
        std::array<Binding, eventCount> Bindings = [] {
            std::array<Binding, eventCount> bindings{};
            bindings[static_cast<std::size_t>(Event::Hover)].Enabled = false;
            return bindings;
        }();
        bool operator==(const Settings&) const = default;
    };

    inline int Priority(Event event)
    {
        switch (event) {
        case Event::Open: case Event::Close: return 5;
        case Event::Back: return 4;
        case Event::ToggleOn: case Event::ToggleOff: return 3;
        case Event::Activate: return 2;
        case Event::Navigate: return 1;
        default: return 0;
        }
    }
}
