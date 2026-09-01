#include "api/PluginInterface.h"
#include "appearance/FontManager.h"
#include "input/InputEventManager.h"
#include "rendering/RenderHooks.h"
#include "runtime/EventManager.h"
#include "runtime/HudManager.h"
#include "runtime/PanelRegistry.h"
#include "runtime/WindowManager.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace
{
	using namespace SFSEMenuFramework;
	std::atomic<bool> consumerInterfacePublished{ false };

	template <class Output>
	[[nodiscard]] bool RejectRendererRegistration(Output* a_output) noexcept
	{
		// This acquire load is the Pending/Failed registration boundary.
		// QueryInterface stays published for renderer-independent input and
		// unregister operations.
		if (!RenderHooks::HasTerminalRendererFailure()) {
			return false;
		}
		if (a_output) {
			*a_output = {};
		}
		return true;
	}

	[[nodiscard]] Model::RegistrationResult __stdcall RegisterPanelAPI(
		const Model::PanelRegistration* a_registration,
		Model::PanelHandle*              a_handle) noexcept
	{
		if (RejectRendererRegistration(a_handle)) {
			return Model::RegistrationResult::InterfaceUnavailable;
		}
		return PanelRegistry::Register(a_registration, a_handle);
	}

	[[nodiscard]] Model::RegistrationResult __stdcall RegisterWindowAPI(
		const Model::WindowRegistration* a_registration,
		Model::WindowInterface**          a_window) noexcept
	{
		if (RejectRendererRegistration(a_window)) {
			return Model::RegistrationResult::InterfaceUnavailable;
		}
		return WindowManager::RegisterWindow(a_registration, a_window);
	}

	[[nodiscard]] Model::RegistrationResult __stdcall RegisterEventAPI(
		const Model::EventRegistration* a_registration,
		Model::EventHandle*              a_handle) noexcept
	{
		if (RejectRendererRegistration(a_handle)) {
			return Model::RegistrationResult::InterfaceUnavailable;
		}
		return EventManager::Register(a_registration, a_handle);
	}

	[[nodiscard]] Model::RegistrationResult __stdcall RegisterHudElementAPI(
		const Model::HudElementRegistration* a_registration,
		Model::HudElementHandle*              a_handle) noexcept
	{
		if (RejectRendererRegistration(a_handle)) {
			return Model::RegistrationResult::InterfaceUnavailable;
		}
		return HudManager::Register(a_registration, a_handle);
	}

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
		.RegisterPanel = &RegisterPanelAPI,
		.RegisterWindow = &RegisterWindowAPI,
		.GetMainWindow = &GetMainWindowAPI,
		.IsAnyBlockingWindowOpened = &WindowManager::IsAnyBlockingWindowOpened,
		.SetHotkeyEnabled = &WindowManager::SetHotkeyEnabled,
		.IsHotkeyEnabled = &WindowManager::IsHotkeyEnabled,
		.RegisterEvent = &RegisterEventAPI,
		.UnregisterEvent = &EventManager::Unregister,
		.RegisterInputEvent = &InputEventManager::Register,
		.UnregisterInputEvent = &InputEventManager::Unregister,
		.RegisterHudElement = &RegisterHudElementAPI,
		.UnregisterHudElement = &HudManager::Unregister,
		.PushFont = &PushFontAPI,
		.PopFont = &PopFontAPI
	};
}

namespace SFSEMenuFramework::PluginInterface
{
	void Publish() noexcept
	{
		consumerInterfacePublished.store(true, std::memory_order_release);
	}
}

extern "C" __declspec(dllexport)
	const SFSEMenuFramework::Model::Interface* __stdcall
	SFSEMenuFramework_QueryInterface(std::uint32_t a_version) noexcept
{
	using namespace SFSEMenuFramework::Model;
	return a_version == INTERFACE_VERSION &&
			consumerInterfacePublished.load(std::memory_order_acquire) ?
		&consumerInterface : nullptr;
}
