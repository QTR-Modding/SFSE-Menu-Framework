#pragma once

namespace SFSE
{
	class TaskInterface;
}

namespace SFSEMenuFramework::MenuLifecycle
{
	[[nodiscard]] bool InstallEarly(const SFSE::TaskInterface& a_taskInterface);
	[[nodiscard]] bool ActivatePostDataLoad() noexcept;
}
