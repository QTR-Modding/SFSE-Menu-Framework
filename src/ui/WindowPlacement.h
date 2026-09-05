#pragma once

namespace SFSEMenuFramework::WindowPlacement
{
	enum class BuiltInWindow
	{
		Main,
		Settings
	};

	// Called on the render thread, respectively before and immediately after Begin.
	void Apply(BuiltInWindow a_window);
	void Capture(BuiltInWindow a_window);
	void Reset();
	// Flush after all windows render, including frames where the MCP just closed.
	void SavePending(bool a_mainWindowOpen);
}
