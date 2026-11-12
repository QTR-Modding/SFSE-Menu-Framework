#pragma once

namespace SFSE
{
	struct TaskInterface;
}

namespace SFSEMenuFramework::MenuLifecycle
{
	[[nodiscard]] bool InstallEarly(const SFSE::TaskInterface& a_taskInterface);
	[[nodiscard]] bool ActivatePostDataLoad() noexcept;
}
