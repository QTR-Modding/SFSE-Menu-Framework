#pragma once

namespace SFSEMenuFramework::SettingsWindow
{
	void UpdateLifecycle() noexcept;
	[[nodiscard]] bool IsOpen() noexcept;
	void Open() noexcept;
	void Close() noexcept;
	void Render();
}
