#pragma once

namespace SFSEMenuFramework
{
	class McpWindow final
	{
	public:
		static bool Install();

	private:
		static void __stdcall Render();
	};
}
