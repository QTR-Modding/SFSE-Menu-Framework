#pragma once
#include "audio/SoundTypes.h"
#include <optional>

namespace SFSEMenuFramework::Audio
{
    void BeginInteractions(bool enabled);
    void RequestInteraction(Event event);
    std::optional<Event> EndInteractions();
}
