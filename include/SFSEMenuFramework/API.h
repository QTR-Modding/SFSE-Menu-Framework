#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace RE
{
	class InputEvent;
}

namespace SFSEMenuFramework::Model
{
	inline constexpr std::uint32_t INTERFACE_VERSION = 1;
	inline constexpr std::uint32_t IMGUI_SOURCE_REVISION = 0x6F7B5D0E;
	inline constexpr std::uint32_t MAXIMUM_PANEL_ID_LENGTH = 255;
	inline constexpr std::uint32_t MAXIMUM_PANEL_TEXT_LENGTH = 255;
	inline constexpr std::uint32_t MAXIMUM_FONT_NAME_LENGTH = 63;

	using PanelHandle = std::uint64_t;
	using EventHandle = std::uint64_t;
	using InputEventHandle = std::uint64_t;
	using HudElementHandle = std::uint64_t;

	enum class RegistrationResult : std::uint32_t
	{
		Success = 0,
		InterfaceUnavailable,
		InvalidArgument,
		UnsupportedVersion,
		StructureTooSmall,
		ImGuiMismatch,
		DuplicateId,
		RegistryFull,
		OutOfMemory,
		InternalError
	};

	enum class PanelRenderResult : std::uint32_t
	{
		Continue = 0,
		Disable,
		Failed
	};

	enum class EventType : std::uint32_t
	{
		kNone = 0,
		kOpenMenu = 1,
		kCloseMenu = 2,
		kBeforeRender = 3,
		kAfterRender = 4
	};

	struct StringView final
	{
		const char*   Data{ nullptr };
		std::uint32_t Size{ 0 };
	};

	struct ImGuiLayout final
	{
		std::uint32_t StructureSize{ sizeof(ImGuiLayout) };
		std::uint32_t VersionNumber{ 0 };
		std::uint32_t SourceRevision{ 0 };
		std::uint32_t ConfigurationFlags{ 0 };
		std::uint32_t IoSize{ 0 };
		std::uint32_t StyleSize{ 0 };
		std::uint32_t ContextSize{ 0 };
		std::uint32_t Vec2Size{ 0 };
		std::uint32_t Vec4Size{ 0 };
		std::uint32_t DrawVertSize{ 0 };
		std::uint32_t DrawIdxSize{ 0 };
		std::uint32_t DrawCmdSize{ 0 };
		std::uint32_t TextureIdSize{ 0 };
		std::uint32_t WcharSize{ 0 };
	};

	using ImGuiAllocateFunction = void* (*)(std::size_t, void*);
	using ImGuiFreeFunction = void (*)(void*, void*);

	struct RenderContext final
	{
		std::uint32_t         StructureSize{ sizeof(RenderContext) };
		std::uint32_t         InterfaceVersion{ INTERFACE_VERSION };
		std::uint64_t         ContextGeneration{ 0 };
		void*                 ImGuiContext{ nullptr };
		ImGuiAllocateFunction Allocate{ nullptr };
		ImGuiFreeFunction     Free{ nullptr };
		void*                 AllocatorUserData{ nullptr };
	};

	using PanelRenderFunction = PanelRenderResult(__stdcall*)(
		const RenderContext*,
		void*) noexcept;
	using WindowRenderFunction = void(__stdcall*)(
		const RenderContext*,
		void*) noexcept;
	using EventCallback = void(__stdcall*)(EventType) noexcept;
	using InputEventCallback = bool(__stdcall*)(RE::InputEvent*);
	using HudElementCallback = void(__stdcall*)();
	using HudElementRenderFunction = void(__stdcall*)(
		const RenderContext*,
		void*) noexcept;

	class WindowInterface
	{
	public:
		std::atomic<bool> IsOpen{ false };
		std::atomic<bool> BlockUserInput{ true };
	};

	struct PanelRegistration final
	{
		std::uint32_t       StructureSize{ sizeof(PanelRegistration) };
		std::uint32_t       InterfaceVersion{ INTERFACE_VERSION };
		StringView          Id{};
		StringView          Section{};
		StringView          Title{};
		ImGuiLayout         ImGui{};
		PanelRenderFunction Render{ nullptr };
		void*               UserData{ nullptr };
	};

	struct WindowRegistration final
	{
		std::uint32_t        StructureSize{ sizeof(WindowRegistration) };
		std::uint32_t        InterfaceVersion{ INTERFACE_VERSION };
		ImGuiLayout          ImGui{};
		WindowRenderFunction Render{ nullptr };
		void*                UserData{ nullptr };
		std::uint8_t         BlockUserInput{ 1 };
		std::uint8_t         Reserved[7]{};
	};

	struct EventRegistration final
	{
		std::uint32_t StructureSize{ sizeof(EventRegistration) };
		std::uint32_t InterfaceVersion{ INTERFACE_VERSION };
		EventCallback Callback{ nullptr };
		float         Priority{ 0.0F };
		std::uint32_t Reserved{ 0 };
	};

	struct InputEventRegistration final
	{
		std::uint32_t      StructureSize{ sizeof(InputEventRegistration) };
		std::uint32_t      InterfaceVersion{ INTERFACE_VERSION };
		InputEventCallback Callback{ nullptr };
	};

	struct HudElementRegistration final
	{
		std::uint32_t            StructureSize{ sizeof(HudElementRegistration) };
		std::uint32_t            InterfaceVersion{ INTERFACE_VERSION };
		ImGuiLayout              ImGui{};
		HudElementRenderFunction Render{ nullptr };
		void*                    UserData{ nullptr };
	};

	using RegisterPanelFunction = RegistrationResult(__stdcall*)(
		const PanelRegistration*,
		PanelHandle*) noexcept;
	using RegisterWindowFunction = RegistrationResult(__stdcall*)(
		const WindowRegistration*,
		WindowInterface**) noexcept;
	using GetMainWindowFunction = WindowInterface* (__stdcall*)() noexcept;
	using IsAnyBlockingWindowOpenedFunction = bool(__stdcall*)() noexcept;
	using SetHotkeyEnabledFunction = void(__stdcall*)(bool) noexcept;
	using IsHotkeyEnabledFunction = bool(__stdcall*)() noexcept;
	using RegisterEventFunction = RegistrationResult(__stdcall*)(
		const EventRegistration*,
		EventHandle*) noexcept;
	using UnregisterEventFunction = void(__stdcall*)(EventHandle) noexcept;
	using RegisterInputEventFunction = RegistrationResult(__stdcall*)(
		const InputEventRegistration*,
		InputEventHandle*) noexcept;
	using UnregisterInputEventFunction = void(__stdcall*)(
		InputEventHandle) noexcept;
	using RegisterHudElementFunction = RegistrationResult(__stdcall*)(
		const HudElementRegistration*,
		HudElementHandle*) noexcept;
	using UnregisterHudElementFunction = void(__stdcall*)(
		HudElementHandle) noexcept;
	using PushFontFunction = bool(__stdcall*)(const StringView*) noexcept;
	using PopFontFunction = bool(__stdcall*)() noexcept;

	struct Interface final
	{
		std::uint32_t                     StructureSize{ sizeof(Interface) };
		std::uint32_t                     Version{ INTERFACE_VERSION };
		RegisterPanelFunction             RegisterPanel{ nullptr };
		RegisterWindowFunction            RegisterWindow{ nullptr };
		GetMainWindowFunction             GetMainWindow{ nullptr };
		IsAnyBlockingWindowOpenedFunction IsAnyBlockingWindowOpened{ nullptr };
		SetHotkeyEnabledFunction          SetHotkeyEnabled{ nullptr };
		IsHotkeyEnabledFunction           IsHotkeyEnabled{ nullptr };
		RegisterEventFunction             RegisterEvent{ nullptr };
		UnregisterEventFunction           UnregisterEvent{ nullptr };
		RegisterInputEventFunction        RegisterInputEvent{ nullptr };
		UnregisterInputEventFunction      UnregisterInputEvent{ nullptr };
		RegisterHudElementFunction        RegisterHudElement{ nullptr };
		UnregisterHudElementFunction      UnregisterHudElement{ nullptr };
		PushFontFunction                  PushFont{ nullptr };
		PopFontFunction                   PopFont{ nullptr };
	};

	using QueryInterfaceFunction = const Interface* (__stdcall*)(std::uint32_t) noexcept;

	static_assert(sizeof(void*) == 8);
	static_assert(sizeof(StringView) == 16);
	static_assert(sizeof(ImGuiLayout) == 56);
	static_assert(sizeof(RenderContext) == 48);
	static_assert(sizeof(PanelRegistration) == 128);
	static_assert(sizeof(WindowRegistration) == 88);
	static_assert(sizeof(EventRegistration) == 24);
	static_assert(sizeof(InputEventRegistration) == 16);
	static_assert(sizeof(HudElementRegistration) == 80);
	static_assert(sizeof(Interface) == 120);
	static_assert(offsetof(Interface, PushFont) == 104);
	static_assert(std::atomic<bool>::is_always_lock_free);
}
