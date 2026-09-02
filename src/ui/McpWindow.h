#pragma once

#include "api/InternalTypes.h"

namespace SFSEMenuFramework
{
	class McpWindow final
	{
	public:
		static bool Install();

	private:
		static void __stdcall Render(
			const Model::RenderContext& a_context);
	};
}
