#pragma once

#include "api/InternalTypes.h"

namespace SFSEMenuFramework
{
	class McpWindow final
	{
	public:
		static bool Install();
		static bool ConsumeGamepadBack();

	private:
		static void __stdcall Render();
	};
}
