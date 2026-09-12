#include "lifecycle/MenuLifecycle.h"
#include "rendering/PresentOverlay.h"

#include <atomic>

namespace
{
	std::atomic<bool> earlyLifecycleReady{ false };

	void EnsurePresentOverlay()
	{
		static_cast<void>(SFSEMenuFramework::PresentOverlay::EnsureInstalled());
	}

	void OnSFSEMessage(SFSE::MessagingInterface::Message* a_message)
	{
		if (!a_message || a_message->type != SFSE::MessagingInterface::kPostDataLoad) {
			return;
		}
		if (!earlyLifecycleReady.load(std::memory_order_acquire)) {
			return;
		}

		if (!SFSEMenuFramework::MenuLifecycle::ActivatePostDataLoad()) {
			logger::critical("Post-data-load menu ownership activation failed");
		}
	}
}

SFSE_PLUGIN_PRELOAD(const SFSE::LoadInterface*)
{
	return true;
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

	const auto* taskInterface = SFSE::GetTaskInterface();
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

	if (!SFSEMenuFramework::MenuLifecycle::InstallEarly(*taskInterface)) {
		logger::critical(
			"Failed to install the early menu lifecycle; the plugin will remain loaded but inactive");
		return true;
	}
	taskInterface->AddPermanentTask(&EnsurePresentOverlay);

	earlyLifecycleReady.store(true, std::memory_order_release);
	logger::info("Menu input ownership waiting for SFSE post-data-load");
	logger::info("SFSE-MCP direct exports available");
	logger::info(
		"Mod Control Panel starts closed and can open before SFSE post-data-load once rendering is ready");
	return true;
}
