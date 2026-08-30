#include "McpWindow.h"
#include "MenuOwnership.h"
#include "RenderHooks.h"

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
	if (!a_sfse) {
		return false;
	}

	SFSE::Init(a_sfse, {
						   .trampoline = false,
						   .hook = false,
					   });

	const auto runtime = a_sfse->RuntimeVersion();
	if (runtime != SFSE::RUNTIME_SF_1_16_244) {
		logger::critical("Unsupported Starfield runtime {}", runtime);
		return false;
	}

	const auto* taskInterface = SFSE::GetTaskInterface();
	if (!taskInterface) {
		logger::critical("The SFSE task interface is unavailable");
		return false;
	}

	if (!SFSEMenuFramework::McpWindow::Install()) {
		logger::critical("Failed to register the built-in Mod Control Panel window");
		return false;
	}

	if (!SFSEMenuFramework::RenderHooks::Install()) {
		logger::critical("Failed to install the Scaleform render-pass hooks");
		return false;
	}

	SFSEMenuFramework::MenuOwnership::Install(*taskInterface);
	logger::info("Mod Control Panel registered; press F1 to toggle it");
	return true;
}
