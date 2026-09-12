#pragma once

#include <cstdint>

namespace SFSEMenuFramework::StreamlineUIPrototype
{
	[[nodiscard]] bool Install() noexcept;
	[[nodiscard]] bool HasUIRenderForFrame(std::uint32_t a_frame) noexcept;
}
