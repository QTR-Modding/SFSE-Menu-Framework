#include "MenuLifecycle.h"
#include "RenderHooks.h"

#include <SFSEMenuFramework/API.h>

#include <atomic>

namespace
{
	const SFSE::TaskInterface* taskInterface{};
	std::atomic<bool>          initializationComplete{ false };

	void OnSFSEMessage(SFSE::MessagingInterface::Message* a_message)
	{
		if (!a_message || a_message->type != SFSE::MessagingInterface::kPostDataLoad) {
			return;
		}

		if (!initializationComplete.load(std::memory_order_acquire)) {
			return;
		}

		if (!taskInterface) {
			logger::critical("The SFSE task interface is unavailable at post-data-load");
			return;
		}

		if (!SFSEMenuFramework::MenuLifecycle::Install(*taskInterface)) {
			logger::critical("Menu input lifecycle initialization failed");
		}
	}
}

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

	taskInterface = SFSE::GetTaskInterface();
	if (!taskInterface) {
		logger::critical("The SFSE task interface is unavailable");
		return false;
	}

	const auto* messagingInterface = SFSE::GetMessagingInterface();
	if (!messagingInterface) {
		logger::critical("The SFSE messaging interface is unavailable");
		return false;
	}

	if (!messagingInterface->RegisterListener(OnSFSEMessage)) {
		logger::critical("Failed to register the SFSE post-data-load listener");
		return false;
	}

	if (!SFSEMenuFramework::RenderHooks::Install()) {
		logger::critical(
			"Failed to install the Scaleform render-pass hooks; the plugin will remain loaded but inactive");
		return true;
	}

	initializationComplete.store(true, std::memory_order_release);

	logger::info("Menu input lifecycle waiting for SFSE post-data-load");
	logger::info(
		"External panel interface v{} available",
		SFSEMenuFramework::Model::INTERFACE_VERSION);
	logger::info("Mod Control Panel starts closed; controls load at post-data-load");
	return true;
}
