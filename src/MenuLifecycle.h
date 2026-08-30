#pragma once

namespace SFSE
{
	class TaskInterface;
}

namespace SFSEMenuFramework::MenuLifecycle
{
	[[nodiscard]] bool Install(const SFSE::TaskInterface& a_taskInterface);
}
