#include "lifecycle/MenuLifecycle.h"
#include "rendering/RenderHooks.h"

#include <RE/C/CreationRenderer.h>

#include <Windows.h>

#include <atomic>

namespace
{
	std::atomic<bool> earlyLifecycleReady{ false };

	void LogRenderPassDiagnostics()
	{
		using namespace RE::CreationRendererPrivate;
		constexpr auto slot = kExecuteRenderPassVTableIndex;
		const auto imageBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));

		const auto logPass = [imageBase](const char* a_name, auto a_vtableId, auto a_executeId) {
			REL::Relocation<std::uintptr_t> vtable{ a_vtableId };
			REL::Relocation<ExecuteRenderPass_t> target{ a_executeId };
			const auto address = target.address();
			const auto current = *reinterpret_cast<const std::uintptr_t*>(
				vtable.address() + sizeof(std::uintptr_t) * kExecuteRenderPassVTableIndex);
			logger::info(
				"Scaleform {}: target=0x{:X}, RVA=0x{:X}, slot=0x{:X}",
				a_name,
				address,
				address - imageBase,
				current);
		};

		logPass("Begin", ScaleformRenderPass::Begin::VTABLE[0], ScaleformRenderPass::Begin::Execute);
		logPass("End", ScaleformRenderPass::End::VTABLE[0], ScaleformRenderPass::End::Execute);
		logPass(
			"Composite",
			ScaleformRenderPass::Composite::VTABLE[0],
			ScaleformRenderPass::Composite::Execute);
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

	LogRenderPassDiagnostics();

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

	if (!SFSEMenuFramework::RenderHooks::Install()) {
		logger::critical(
			"Failed to install the Scaleform render-pass hooks; the plugin will remain loaded but inactive");
		return true;
	}

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
