#include "lifecycle/MenuLifecycle.h"
#include "rendering/PresentOverlay.h"
#include "rendering/StreamlineUIPrototype.h"

#include <atomic>

#include <spdlog/sinks/basic_file_sink.h>

namespace
{
	void InitializeLog()
	{
		const auto directory = SFSE::GetLogDirectory();
		if (!directory) {
			return;
		}
		std::error_code error;
		std::filesystem::create_directories(*directory, error);
		if (error) {
			logger::warn("Could not create the SFSE log directory: {}", error.message());
			return;
		}
		const auto path = *directory / (std::string(PLUGIN_NAME) + ".log");
		std::shared_ptr<spdlog::logger> output;
		try {
			output = spdlog::basic_logger_mt(PLUGIN_DLL_NAME, path.string(), true);
		} catch (const spdlog::spdlog_ex& failure) {
			logger::warn("Could not open the plugin log: {}", failure.what());
			return;
		}
#ifdef NDEBUG
		output->set_level(spdlog::level::info);
		output->flush_on(spdlog::level::info);
#else
		output->set_level(spdlog::level::debug);
		output->flush_on(spdlog::level::debug);
#endif
		output->set_pattern("[%T.%e] [%=5t] [%L] %v");
		spdlog::set_default_logger(std::move(output));
	}

	std::atomic<bool> earlyLifecycleReady{ false };

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

extern "C" __declspec(dllexport) bool SFSEPlugin_Preload(const SFSE::LoadInterface*)
{
	return true;
}

extern "C" __declspec(dllexport) constinit SFSE::PluginVersionData SFSEPlugin_Version{
	1,
	PLUGIN_PACKED_VERSION,
	PLUGIN_NAME,
	PLUGIN_AUTHOR,
	1U << 2,
	1U << 3,
	{ SFSE::RUNTIME_SF_1_16_244 },
	0, 0, 0
};

extern "C" __declspec(dllexport) bool SFSEPlugin_Load(const SFSE::LoadInterface* a_sfse)
{
	if (!a_sfse) {
		return false;
	}

	InitializeLog();
	const auto runtime = a_sfse->RuntimeVersion();
	if (runtime != SFSE::RUNTIME_SF_1_16_244) {
		logger::critical("Unsupported Starfield runtime {}", runtime);
		return false;
	}

	if (!SFSE::Init(*a_sfse, 0)) {
		logger::critical("Could not initialize SFSE and the Address Library");
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

	// SFSE now owns our callback, so later failures must leave the DLL loaded.
	// Install rendering hooks here, before rendering starts.
	if (!SFSEMenuFramework::PresentOverlay::EnsureInstalled()) {
		logger::critical("Failed to install frame presentation; the plugin will remain loaded but inactive");
		return true;
	}
	static_cast<void>(SFSEMenuFramework::StreamlineUIPrototype::Install());

	if (!SFSEMenuFramework::MenuLifecycle::InstallEarly(*taskInterface)) {
		logger::critical(
			"Failed to install the early menu lifecycle; the plugin will remain loaded but inactive");
		return true;
	}

	earlyLifecycleReady.store(true, std::memory_order_release);
	logger::info("Menu input ownership waiting for SFSE post-data-load");
	logger::info("SFSE-MCP direct exports available");
	logger::info(
		"Mod Control Panel starts closed and can open before SFSE post-data-load once rendering is ready");
	return true;
}
