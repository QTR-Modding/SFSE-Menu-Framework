#pragma once

#include <RE/C/CreationRenderer.h>
#include <REL/ASM.h>

namespace SFSEMenuFramework
{
#pragma pack(push, 1)
	// At PresentRequest::Execute + 0x100 in 1.16.244, R15 owns the request.
	// Preserve the three DXGI arguments and supply its frame index as argument 4.
	struct FramePresentBridge
	{
		explicit FramePresentBridge(std::uintptr_t a_target) : jump(a_target) {}

		std::uint8_t frameArgument[4]{ 0x45, 0x8B, 0x4F,
			static_cast<std::uint8_t>(offsetof(RE::CreationRendererPrivate::PresentRequest, frameIndex)) }; // mov r9d,[r15+50h]
		REL::ASM::JMP14 jump;
	};
#pragma pack(pop)
	static_assert(sizeof(FramePresentBridge) == 18);
}
