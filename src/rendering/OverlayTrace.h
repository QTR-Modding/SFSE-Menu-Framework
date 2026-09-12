#pragma once

namespace SFSEMenuFramework::OverlayTrace
{
	enum Event { Tag, NullTag, Rejected, StateMissing, Duplicate, ImGuiDraw, ImGuiSkipped,
		PresentDraw, FrameUI, FrameFallback, Count };
	void Record(Event event) noexcept;
	void StateRejected(const char* reason) noexcept;
	void Report();
}
