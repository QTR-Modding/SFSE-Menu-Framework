#include "PanelRegistry.h"
#include "WindowManager.h"

namespace
{
	[[nodiscard]] SFSEMenuFramework::Model::RegistrationResult __stdcall RegisterPanel(
		const SFSEMenuFramework::Model::PanelRegistration* a_registration,
		SFSEMenuFramework::Model::PanelHandle*              a_handle) noexcept
	{
		return SFSEMenuFramework::PanelRegistry::Register(a_registration, a_handle);
	}

	[[nodiscard]] SFSEMenuFramework::Model::RegistrationResult __stdcall RegisterWindow(
		const SFSEMenuFramework::Model::WindowRegistration* a_registration,
		SFSEMenuFramework::Model::WindowInterface**          a_window) noexcept
	{
		return SFSEMenuFramework::WindowManager::RegisterWindow(
			a_registration,
			a_window);
	}

	[[nodiscard]] SFSEMenuFramework::Model::WindowInterface* __stdcall GetMainWindow() noexcept
	{
		return SFSEMenuFramework::WindowManager::GetMainWindow();
	}

	[[nodiscard]] bool __stdcall IsAnyBlockingWindowOpened() noexcept
	{
		return SFSEMenuFramework::WindowManager::IsAnyBlockingWindowOpened();
	}

	void __stdcall SetHotkeyEnabled(bool a_enabled) noexcept
	{
		SFSEMenuFramework::WindowManager::SetHotkeyEnabled(a_enabled);
	}

	[[nodiscard]] bool __stdcall IsHotkeyEnabled() noexcept
	{
		return SFSEMenuFramework::WindowManager::IsHotkeyEnabled();
	}

	const SFSEMenuFramework::Model::Interface interfaceV1{
		.StructureSize = sizeof(SFSEMenuFramework::Model::Interface),
		.Version = SFSEMenuFramework::Model::INTERFACE_VERSION,
		.RegisterPanel = &RegisterPanel
	};

	const SFSEMenuFramework::Model::InterfaceV2 interfaceV2{
		.StructureSize = sizeof(SFSEMenuFramework::Model::InterfaceV2),
		.Version = SFSEMenuFramework::Model::INTERFACE_VERSION_2,
		.RegisterPanel = &RegisterPanel,
		.RegisterWindow = &RegisterWindow,
		.GetMainWindow = &GetMainWindow,
		.IsAnyBlockingWindowOpened = &IsAnyBlockingWindowOpened,
		.SetHotkeyEnabled = &SetHotkeyEnabled,
		.IsHotkeyEnabled = &IsHotkeyEnabled
	};
}

extern "C" __declspec(dllexport)
	const SFSEMenuFramework::Model::Interface* __stdcall
	SFSEMenuFramework_QueryInterface(std::uint32_t a_requestedVersion) noexcept
{
	switch (a_requestedVersion) {
	case SFSEMenuFramework::Model::INTERFACE_VERSION:
		return &interfaceV1;
	case SFSEMenuFramework::Model::INTERFACE_VERSION_2:
		return reinterpret_cast<const SFSEMenuFramework::Model::Interface*>(
			&interfaceV2);
	default:
		return nullptr;
	}
}
