#include "appearance/FontManager.h"
#include "input/InputEventManager.h"
#include "runtime/EventManager.h"
#include "runtime/HudManager.h"
#include "runtime/PanelRegistry.h"
#include "runtime/WindowManager.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Direct client exports port the framework boundary from SKSE Menu Framework
// through accepted base 8a366c4a3db7317655cec8379e48c43140c9fe7d. Client callbacks call the
// cimgui exports compiled into this DLL, so the real ImGui context never leaves
// the framework.
namespace
{
	using namespace SFSEMenuFramework;

	using DirectRenderFunction = Model::ClientWindowRenderFunction;
	using DirectInputEventCallback = Model::InputEventCallback;
	using DirectHudElementCallback = Model::ClientHudElementRenderFunction;

	static_assert(sizeof(DirectRenderFunction) == sizeof(void*));
	static_assert(sizeof(DirectHudElementCallback) == sizeof(void*));
	static_assert(sizeof(Model::WindowInterface) == 2 * sizeof(std::atomic<bool>));
	static_assert(offsetof(Model::WindowInterface, IsOpen) == 0);
	static_assert(
		offsetof(Model::WindowInterface, BlockUserInput) ==
		sizeof(std::atomic<bool>));

}

extern "C" __declspec(dllexport) void AddSectionItem(
	const char* a_path,
	DirectRenderFunction a_render)
{
	if (a_path && a_render) {
		static_cast<void>(
			SFSEMenuFramework::PanelRegistry::RegisterDirect(a_path, a_render));
	}
}

extern "C" __declspec(dllexport) bool RenameSection(
	const char* a_path,
	const char* a_newName)
{
	return a_path && a_newName &&
		SFSEMenuFramework::PanelRegistry::Rename(a_path, a_newName);
}

extern "C" __declspec(dllexport) bool DeleteSection(const char* a_path)
{
	return a_path && SFSEMenuFramework::PanelRegistry::Delete(a_path);
}

extern "C" __declspec(dllexport)
	SFSEMenuFramework::Model::WindowInterface* AddWindow(
		DirectRenderFunction a_render)
{
	using namespace SFSEMenuFramework;
	if (!a_render) {
		return nullptr;
	}
	return WindowManager::AddExternalWindow(a_render);
}

extern "C" __declspec(dllexport)
	SFSEMenuFramework::Model::WindowInterface* AddWindowWithView(
		DirectRenderFunction a_render,
		const char*)
{
	return AddWindow(a_render);
}

extern "C" __declspec(dllexport) void PushDefault()
{
	static_cast<void>(SFSEMenuFramework::FontManager::PushDefaultFont());
}

extern "C" __declspec(dllexport) void PushSolid()
{
	static_cast<void>(
		SFSEMenuFramework::FontManager::PushFont("fa-solid-900.ttf"));
}

extern "C" __declspec(dllexport) void PushRegular()
{
	static_cast<void>(
		SFSEMenuFramework::FontManager::PushFont("fa-regular-400.ttf"));
}

extern "C" __declspec(dllexport) void PushBrands()
{
	static_cast<void>(
		SFSEMenuFramework::FontManager::PushFont("fa-brands-400.ttf"));
}

extern "C" __declspec(dllexport) void PushFont(const char* a_name)
{
	if (a_name) {
		static_cast<void>(SFSEMenuFramework::FontManager::PushFont(a_name));
	}
}

extern "C" __declspec(dllexport) void Pop()
{
	static_cast<void>(SFSEMenuFramework::FontManager::PopFont());
}

extern "C" __declspec(dllexport) std::int64_t RegisterInputEvent(
	DirectInputEventCallback a_callback)
{
	return static_cast<std::int64_t>(
		SFSEMenuFramework::InputEventManager::Register(a_callback));
}

extern "C" __declspec(dllexport) void UnregisterInputEvent(
	std::uint64_t a_handle)
{
	SFSEMenuFramework::InputEventManager::Unregister(a_handle);
}

extern "C" __declspec(dllexport) std::int64_t RegisterHudElement(
	DirectHudElementCallback a_callback)
{
	return static_cast<std::int64_t>(
		SFSEMenuFramework::HudManager::Register(a_callback));
}

extern "C" __declspec(dllexport) void UnregisterHudElement(
	std::uint64_t a_handle)
{
	SFSEMenuFramework::HudManager::Unregister(a_handle);
}

extern "C" __declspec(dllexport) bool IsAnyBlockingWindowOpened()
{
	return SFSEMenuFramework::WindowManager::IsAnyBlockingWindowOpened();
}

extern "C" __declspec(dllexport) std::int64_t RegisterEventPriority(
	SFSEMenuFramework::Model::EventCallback a_callback,
	float a_priority)
{
	return static_cast<std::int64_t>(
		SFSEMenuFramework::EventManager::Register(a_callback, a_priority));
}

extern "C" __declspec(dllexport) std::int64_t RegisterEvent(
	SFSEMenuFramework::Model::EventCallback a_callback)
{
	return RegisterEventPriority(a_callback, 0.0F);
}

extern "C" __declspec(dllexport) void UnregisterEvent(std::int64_t a_handle)
{
	if (a_handle > 0) {
		SFSEMenuFramework::EventManager::Unregister(
			static_cast<SFSEMenuFramework::Model::EventHandle>(a_handle));
	}
}

extern "C" __declspec(dllexport) float GetMenuFrameworkVersion()
{
	// Preserve the SKSE-MCP-compatible float export as an informational release
	// projection. Capability checks belong to GetMenuFrameworkAPIVersion().
	const auto version = SFSE::GetPluginVersion();
	float minorScale = 10.0F;
	for (auto remaining = version.minor(); remaining >= 10; remaining /= 10) {
		minorScale *= 10.0F;
	}
	return static_cast<float>(version.major()) +
		static_cast<float>(version.minor()) / minorScale;
}

extern "C" __declspec(dllexport) std::uint32_t GetMenuFrameworkAPIVersion()
{
	return 1;
}

extern "C" __declspec(dllexport)
	SFSEMenuFramework::Model::WindowInterface* GetMainWindow()
{
	return SFSEMenuFramework::WindowManager::GetMainWindow();
}

extern "C" __declspec(dllexport) void SetHotkeyEnabled(bool a_enabled)
{
	SFSEMenuFramework::WindowManager::SetHotkeyEnabled(a_enabled);
}

extern "C" __declspec(dllexport) bool IsHotkeyEnabled()
{
	return SFSEMenuFramework::WindowManager::IsHotkeyEnabled();
}
