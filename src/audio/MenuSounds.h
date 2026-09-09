#pragma once
#include "audio/SoundTypes.h"
#include <string>

namespace SFSEMenuFramework::Audio
{
    void BeginFrame();
    void EndFrame();
    void Preview(Event event);
    void Reload();
    std::string GetError();
}
