#include "appearance/FontManager.h"
#include "input/InputEventManager.h"
#include "runtime/EventManager.h"
#include "runtime/HudManager.h"
#include "runtime/PanelRegistry.h"
#include "runtime/WindowManager.h"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace
{
	using namespace SFSEMenuFramework;

	[[nodiscard]] Model::WindowInterface* __stdcall GetMainWindowAPI() noexcept
	{
		return WindowManager::GetMainWindow();
	}

	[[nodiscard]] bool __stdcall PushFontAPI(
		const Model::StringView* a_name) noexcept
	{
		if (!a_name || !a_name->Data || a_name->Size == 0 ||
			a_name->Size > Model::MAXIMUM_FONT_NAME_LENGTH ||
			std::memchr(a_name->Data, '\0', a_name->Size)) {
			return false;
		}
		return FontManager::PushFont(
			std::string_view{ a_name->Data, a_name->Size });
	}

	[[nodiscard]] bool __stdcall PopFontAPI() noexcept
	{
		return FontManager::PopFont();
	}

	const Model::Interface consumerInterface{
		.StructureSize = sizeof(Model::Interface),
		.Version = Model::INTERFACE_VERSION,
		.RegisterPanel = &PanelRegistry::Register,
		.RegisterWindow = &WindowManager::RegisterWindow,
		.GetMainWindow = &GetMainWindowAPI,
		.IsAnyBlockingWindowOpened = &WindowManager::IsAnyBlockingWindowOpened,
		.SetHotkeyEnabled = &WindowManager::SetHotkeyEnabled,
		.IsHotkeyEnabled = &WindowManager::IsHotkeyEnabled,
		.RegisterEvent = &EventManager::Register,
		.UnregisterEvent = &EventManager::Unregister,
		.RegisterInputEvent = &InputEventManager::Register,
		.UnregisterInputEvent = &InputEventManager::Unregister,
		.RegisterHudElement = &HudManager::Register,
		.UnregisterHudElement = &HudManager::Unregister,
		.PushFont = &PushFontAPI,
		.PopFont = &PopFontAPI
	};
}

extern "C" __declspec(dllexport)
	const SFSEMenuFramework::Model::Interface* __stdcall
	SFSEMenuFramework_QueryInterface(std::uint32_t a_version) noexcept
{
	using namespace SFSEMenuFramework::Model;
	return a_version == INTERFACE_VERSION ? &consumerInterface : nullptr;
}
