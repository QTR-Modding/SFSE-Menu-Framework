#pragma once

// Host-internal registration and callback types. Client mods use SFSE-MCP.

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
	inline constexpr std::uint32_t IMGUI_SOURCE_REVISION = 0x6D948AB4;

	using EventHandle = std::uint64_t;
	using InputEventHandle = std::uint64_t;
	using HudElementHandle = std::uint64_t;

	enum class RegistrationResult : std::uint32_t
	{
		Success = 0,
		InvalidArgument,
		UnsupportedVersion,
		StructureTooSmall,
		ImGuiMismatch,
		RegistryFull,
		OutOfMemory,
		InternalError
	};

	enum EventType : std::uint32_t
	{
		kNone = 0,
		kOpenMenu = 1,
		kCloseMenu = 2,
		kBeforeRender = 3,
		kAfterRender = 4
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

	using WindowRenderFunction = void(__stdcall*)(
		const RenderContext*,
		void*) noexcept;
	using EventCallback = void(__stdcall*)(EventType);
	using InputEventCallback = bool(__stdcall*)(RE::InputEvent*);
	using HudElementRenderFunction = void(__stdcall*)(
		const RenderContext*,
		void*) noexcept;

	class WindowInterface
	{
	public:
		std::atomic<bool> IsOpen{ false };
		std::atomic<bool> BlockUserInput{ true };
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

	static_assert(sizeof(void*) == 8);
	static_assert(sizeof(ImGuiLayout) == 56);
	static_assert(sizeof(RenderContext) == 48);
	static_assert(sizeof(WindowRegistration) == 88);
	static_assert(sizeof(EventRegistration) == 24);
	static_assert(sizeof(InputEventRegistration) == 16);
	static_assert(sizeof(HudElementRegistration) == 80);
	static_assert(std::atomic<bool>::is_always_lock_free);
}
