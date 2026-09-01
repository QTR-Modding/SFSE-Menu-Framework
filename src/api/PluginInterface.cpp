#include "runtime/EventManager.h"
#include "runtime/PanelRegistry.h"
#include "runtime/WindowManager.h"

#include <cstdint>

namespace
{
	using namespace SFSEMenuFramework;

	[[nodiscard]] Model::WindowInterface* __stdcall GetMainWindowAPI() noexcept
	{
		return WindowManager::GetMainWindow();
	}

	const Model::Interface interfaceV1{
		.StructureSize = sizeof(Model::Interface),
		.Version = Model::INTERFACE_VERSION,
		.RegisterPanel = &PanelRegistry::Register
	};
	const Model::InterfaceV2 interfaceV2{
		.StructureSize = sizeof(Model::InterfaceV2),
		.Version = Model::INTERFACE_VERSION_2,
		.RegisterPanel = &PanelRegistry::Register,
		.RegisterWindow = &WindowManager::RegisterWindow,
		.GetMainWindow = &GetMainWindowAPI,
		.IsAnyBlockingWindowOpened = &WindowManager::IsAnyBlockingWindowOpened,
		.SetHotkeyEnabled = &WindowManager::SetHotkeyEnabled,
		.IsHotkeyEnabled = &WindowManager::IsHotkeyEnabled
	};
	const Model::InterfaceV3 interfaceV3{
		.StructureSize = sizeof(Model::InterfaceV3),
		.Version = Model::INTERFACE_VERSION_3,
		.RegisterPanel = &PanelRegistry::Register,
		.RegisterWindow = &WindowManager::RegisterWindow,
		.GetMainWindow = &GetMainWindowAPI,
		.IsAnyBlockingWindowOpened = &WindowManager::IsAnyBlockingWindowOpened,
		.SetHotkeyEnabled = &WindowManager::SetHotkeyEnabled,
		.IsHotkeyEnabled = &WindowManager::IsHotkeyEnabled,
		.RegisterEvent = &EventManager::Register,
		.UnregisterEvent = &EventManager::Unregister
	};
}

extern "C" __declspec(dllexport)
	const SFSEMenuFramework::Model::Interface* __stdcall
	SFSEMenuFramework_QueryInterface(std::uint32_t a_version) noexcept
{
	using namespace SFSEMenuFramework::Model;
	switch (a_version) {
	case INTERFACE_VERSION:
		return &interfaceV1;
	case INTERFACE_VERSION_2:
		return reinterpret_cast<const Interface*>(&interfaceV2);
	case INTERFACE_VERSION_3:
		return reinterpret_cast<const Interface*>(&interfaceV3);
	default:
		return nullptr;
	}
}
