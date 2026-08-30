#pragma once

#include <cstddef>
#include <cstdint>

namespace SFSEMenuFramework::Model
{
	inline constexpr std::uint32_t INTERFACE_VERSION = 1;
	inline constexpr std::uint32_t IMGUI_SOURCE_REVISION = 0x6F7B5D0E;
	inline constexpr std::uint32_t MAXIMUM_PANEL_ID_LENGTH = 255;
	inline constexpr std::uint32_t MAXIMUM_PANEL_TEXT_LENGTH = 255;

	using PanelHandle = std::uint64_t;

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

	using RegisterPanelFunction = RegistrationResult(__stdcall*)(
		const PanelRegistration*,
		PanelHandle*) noexcept;

	struct Interface final
	{
		std::uint32_t         StructureSize{ sizeof(Interface) };
		std::uint32_t         Version{ INTERFACE_VERSION };
		RegisterPanelFunction RegisterPanel{ nullptr };
	};

	using QueryInterfaceFunction = const Interface* (__stdcall*)(std::uint32_t) noexcept;

	static_assert(sizeof(void*) == 8);
	static_assert(sizeof(StringView) == 16);
	static_assert(sizeof(ImGuiLayout) == 56);
	static_assert(sizeof(RenderContext) == 48);
	static_assert(sizeof(PanelRegistration) == 128);
	static_assert(sizeof(Interface) == 16);
}
