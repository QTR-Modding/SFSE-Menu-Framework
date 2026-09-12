#pragma once

namespace SFSEMenuFramework::OverlayTrace
{
	enum Event { Tag, NullTag, Rejected, StateMissing, Duplicate, ImGuiDraw, ImGuiSkipped,
		PresentDraw, RouteOn, RouteOff, Timeout, Count };
	void Record(Event event) noexcept;
	void StateRejected(const char* reason) noexcept;
	void Report();
}
