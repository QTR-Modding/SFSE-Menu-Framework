#include "appearance/FontManager.h"
#include "input/InputEventManager.h"
#include "runtime/EventManager.h"
#include "runtime/HudManager.h"
#include "runtime/PanelRegistry.h"
#include "runtime/WindowManager.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Direct client exports port the framework boundary from SKSE Menu Framework
// commit 928e01ab459822a8d233ab99f0419ea1de23c775. Client callbacks call the
// cimgui exports compiled into this DLL, so the real ImGui context never leaves
// the framework.
namespace
{
	using namespace SFSEMenuFramework;

	using DirectRenderFunction = void(__stdcall*)();
	using DirectInputEventCallback = bool(__stdcall*)(RE::InputEvent*);
	using DirectHudElementCallback = void(__stdcall*)();

	static_assert(sizeof(DirectRenderFunction) == sizeof(void*));
	static_assert(sizeof(DirectHudElementCallback) == sizeof(void*));
	static_assert(sizeof(Model::WindowInterface) == 2 * sizeof(std::atomic<bool>));
	static_assert(offsetof(Model::WindowInterface, IsOpen) == 0);
	static_assert(
		offsetof(Model::WindowInterface, BlockUserInput) ==
		sizeof(std::atomic<bool>));

	[[nodiscard]] Model::ImGuiLayout GetHostImGuiLayout() noexcept
	{
		return Model::ImGuiLayout{
			.StructureSize = sizeof(Model::ImGuiLayout),
			.VersionNumber = IMGUI_VERSION_NUM,
			.SourceRevision = Model::IMGUI_SOURCE_REVISION,
			.ConfigurationFlags = 0,
			.IoSize = sizeof(ImGuiIO),
			.StyleSize = sizeof(ImGuiStyle),
			.ContextSize = sizeof(ImGuiContext),
			.Vec2Size = sizeof(ImVec2),
			.Vec4Size = sizeof(ImVec4),
			.DrawVertSize = sizeof(ImDrawVert),
			.DrawIdxSize = sizeof(ImDrawIdx),
			.DrawCmdSize = sizeof(ImDrawCmd),
			.TextureIdSize = sizeof(ImTextureID),
			.WcharSize = sizeof(ImWchar)
		};
	}

	void __stdcall RenderWindow(
		const Model::RenderContext*,
		void* a_callback) noexcept
	{
		const auto callback =
			std::bit_cast<DirectRenderFunction>(a_callback);
		if (callback) {
			callback();
		}
	}

	void __stdcall RenderHudElement(
		const Model::RenderContext*,
		void* a_callback) noexcept
	{
		const auto callback =
			std::bit_cast<DirectHudElementCallback>(a_callback);
		if (callback) {
			callback();
		}
	}
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

extern "C" __declspec(dllexport)
	SFSEMenuFramework::Model::WindowInterface* AddWindow(
		DirectRenderFunction a_render)
{
	using namespace SFSEMenuFramework;
	if (!a_render) {
		return nullptr;
	}
	const Model::WindowRegistration registration{
		.StructureSize = sizeof(Model::WindowRegistration),
		.InterfaceVersion = Model::INTERFACE_VERSION,
		.ImGui = GetHostImGuiLayout(),
		.Render = &RenderWindow,
		.UserData = std::bit_cast<void*>(a_render),
		.BlockUserInput = 1
	};
	Model::WindowInterface* window{};
	return WindowManager::RegisterWindow(&registration, &window) ==
			Model::RegistrationResult::Success ?
		window : nullptr;
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
	using namespace SFSEMenuFramework;
	const Model::InputEventRegistration registration{
		.StructureSize = sizeof(Model::InputEventRegistration),
		.InterfaceVersion = Model::INTERFACE_VERSION,
		.Callback = a_callback
	};
	Model::InputEventHandle handle{};
	return InputEventManager::Register(&registration, &handle) ==
			Model::RegistrationResult::Success ?
		static_cast<std::int64_t>(handle) : 0;
}

extern "C" __declspec(dllexport) void UnregisterInputEvent(
	std::uint64_t a_handle)
{
	SFSEMenuFramework::InputEventManager::Unregister(a_handle);
}

extern "C" __declspec(dllexport) std::int64_t RegisterHudElement(
	DirectHudElementCallback a_callback)
{
	using namespace SFSEMenuFramework;
	if (!a_callback) {
		return 0;
	}
	const Model::HudElementRegistration registration{
		.StructureSize = sizeof(Model::HudElementRegistration),
		.InterfaceVersion = Model::INTERFACE_VERSION,
		.ImGui = GetHostImGuiLayout(),
		.Render = &RenderHudElement,
		.UserData = std::bit_cast<void*>(a_callback)
	};
	Model::HudElementHandle handle{};
	return HudManager::Register(&registration, &handle) ==
			Model::RegistrationResult::Success ?
		static_cast<std::int64_t>(handle) : 0;
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
	using namespace SFSEMenuFramework;
	const Model::EventRegistration registration{
		.StructureSize = sizeof(Model::EventRegistration),
		.InterfaceVersion = Model::INTERFACE_VERSION,
		.Callback = a_callback,
		.Priority = a_priority
	};
	Model::EventHandle handle{};
	return EventManager::Register(&registration, &handle) ==
			Model::RegistrationResult::Success ?
		static_cast<std::int64_t>(handle) : 0;
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
	return 3.7F;
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
