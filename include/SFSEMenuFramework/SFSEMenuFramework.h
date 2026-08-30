#pragma once

#include <SFSEMenuFramework/API.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <Windows.h>

#include <bit>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>

namespace SFSEMenuFramework
{
	using RenderFunction = void(__stdcall*)() noexcept;
	static_assert(
		IMGUI_VERSION_NUM == 19080,
		"SFSE Menu Framework consumers require Dear ImGui 1.90.8");

	namespace Detail
	{
		struct ConsumerPanel final
		{
			RenderFunction Render{ nullptr };
		};

		inline thread_local std::string currentSection;

		[[nodiscard]] constexpr std::uint32_t GetImGuiConfigurationFlags() noexcept
		{
			std::uint32_t flags{};
#if defined(IMGUI_DISABLE) || defined(IMGUI_DISABLE_OBSOLETE_FUNCTIONS) || \
	defined(IMGUI_DISABLE_OBSOLETE_KEYIO) || defined(IMGUI_DISABLE_DEBUG_TOOLS) || \
	defined(IMGUI_DISABLE_DEFAULT_ALLOCATORS)
			flags |= 1U << 0U;
#endif
#if defined(IMGUI_USE_BGRA_PACKED_COLOR)
			flags |= 1U << 1U;
#endif
#if defined(IMGUI_USE_WCHAR32)
			flags |= 1U << 2U;
#endif
#if defined(IMGUI_OVERRIDE_DRAWVERT_STRUCT_LAYOUT) || defined(ImTextureID) || \
	defined(ImDrawIdx) || defined(ImDrawCallback)
			flags |= 1U << 3U;
#endif
#if defined(IMGUI_USER_CONFIG) || defined(IMGUI_INCLUDE_IMGUI_USER_H)
			flags |= 1U << 4U;
#endif
#if defined(IMGUI_ENABLE_FREETYPE) || defined(IMGUI_ENABLE_FREETYPE_LUNASVG)
			flags |= 1U << 5U;
#endif
			return flags;
		}

		static_assert(
			GetImGuiConfigurationFlags() == 0,
			"SFSE Menu Framework consumers require the pinned default ImGui configuration");

		[[nodiscard]] inline const Model::Interface* RequestInterface() noexcept
		{
			const auto module = ::GetModuleHandleW(L"SFSEMenuFramework.dll");
			if (!module) {
				return nullptr;
			}

			const auto procedure =
				::GetProcAddress(module, "SFSEMenuFramework_QueryInterface");
			if (!procedure) {
				return nullptr;
			}

			static_assert(sizeof(procedure) == sizeof(Model::QueryInterfaceFunction));
			const auto query = std::bit_cast<Model::QueryInterfaceFunction>(procedure);
			const auto* interface = query(Model::INTERFACE_VERSION);
			if (!interface ||
				interface->StructureSize < sizeof(Model::Interface) ||
				interface->Version != Model::INTERFACE_VERSION ||
				!interface->RegisterPanel) {
				return nullptr;
			}

			return interface;
		}

		[[nodiscard]] inline Model::ImGuiLayout GetImGuiLayout() noexcept
		{
			return Model::ImGuiLayout{
				.StructureSize = sizeof(Model::ImGuiLayout),
				.VersionNumber = IMGUI_VERSION_NUM,
				.SourceRevision = Model::IMGUI_SOURCE_REVISION,
				.ConfigurationFlags = GetImGuiConfigurationFlags(),
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

		[[nodiscard]] inline Model::PanelRenderResult __stdcall RenderPanel(
			const Model::RenderContext* a_context,
			void*                       a_userData) noexcept
		{
			if (!a_context ||
				a_context->StructureSize < sizeof(Model::RenderContext) ||
				a_context->InterfaceVersion != Model::INTERFACE_VERSION ||
				!a_context->ImGuiContext ||
				!a_context->Allocate ||
				!a_context->Free ||
				!a_userData) {
				return Model::PanelRenderResult::Failed;
			}

			static_assert(std::is_same_v<Model::ImGuiAllocateFunction, ImGuiMemAllocFunc>);
			static_assert(std::is_same_v<Model::ImGuiFreeFunction, ImGuiMemFreeFunc>);

			auto* const previousContext = ImGui::GetCurrentContext();
			ImGuiMemAllocFunc previousAllocate{};
			ImGuiMemFreeFunc previousFree{};
			void* previousAllocatorUserData{};
			ImGui::GetAllocatorFunctions(
				&previousAllocate,
				&previousFree,
				&previousAllocatorUserData);

			ImGui::SetAllocatorFunctions(
				a_context->Allocate,
				a_context->Free,
				a_context->AllocatorUserData);
			ImGui::SetCurrentContext(
				static_cast<ImGuiContext*>(a_context->ImGuiContext));

			static_cast<ConsumerPanel*>(a_userData)->Render();

			ImGui::SetCurrentContext(previousContext);
			ImGui::SetAllocatorFunctions(
				previousAllocate,
				previousFree,
				previousAllocatorUserData);
			return Model::PanelRenderResult::Continue;
		}

		[[nodiscard]] inline bool IsValidText(
			std::string_view a_text,
			std::uint32_t    a_maximumLength) noexcept
		{
			return !a_text.empty() &&
			       a_text.size() <= a_maximumLength &&
			       a_text.find('\0') == std::string_view::npos &&
			       a_text.find('\x1F') == std::string_view::npos;
		}

		[[nodiscard]] inline Model::StringView ToModelString(
			std::string_view a_text) noexcept
		{
			return Model::StringView{
				.Data = a_text.data(),
				.Size = static_cast<std::uint32_t>(a_text.size())
			};
		}
	}

	[[nodiscard]] inline bool IsInstalled() noexcept
	{
		return Detail::RequestInterface() != nullptr;
	}

	[[nodiscard]] inline bool SetSection(std::string_view a_section)
	{
		if (!Detail::IsValidText(
				a_section,
				Model::MAXIMUM_PANEL_TEXT_LENGTH)) {
			return false;
		}

		try {
			Detail::currentSection.assign(a_section);
			return true;
		} catch (const std::bad_alloc&) {
			return false;
		}
	}

	[[nodiscard]] inline Model::RegistrationResult AddSectionItem(
		std::string_view   a_title,
		RenderFunction     a_renderFunction,
		Model::PanelHandle* a_handle = nullptr)
	{
		if (a_handle) {
			*a_handle = 0;
		}

		if (!a_renderFunction ||
			!Detail::IsValidText(
				Detail::currentSection,
				Model::MAXIMUM_PANEL_TEXT_LENGTH) ||
			!Detail::IsValidText(
				a_title,
				Model::MAXIMUM_PANEL_TEXT_LENGTH)) {
			return Model::RegistrationResult::InvalidArgument;
		}

		const auto* interface = Detail::RequestInterface();
		if (!interface) {
			return Model::RegistrationResult::InterfaceUnavailable;
		}

		try {
			std::string id;
			id.reserve(Detail::currentSection.size() + a_title.size() + 1);
			id.append(Detail::currentSection);
			id.push_back('\x1F');
			id.append(a_title);
			if (id.size() > Model::MAXIMUM_PANEL_ID_LENGTH) {
				return Model::RegistrationResult::InvalidArgument;
			}

			auto* consumerPanel =
				new (std::nothrow) Detail::ConsumerPanel{ a_renderFunction };
			if (!consumerPanel) {
				return Model::RegistrationResult::OutOfMemory;
			}

			const Model::PanelRegistration registration{
				.StructureSize = sizeof(Model::PanelRegistration),
				.InterfaceVersion = Model::INTERFACE_VERSION,
				.Id = Detail::ToModelString(id),
				.Section = Detail::ToModelString(Detail::currentSection),
				.Title = Detail::ToModelString(a_title),
				.ImGui = Detail::GetImGuiLayout(),
				.Render = &Detail::RenderPanel,
				.UserData = consumerPanel
			};

			const auto result = interface->RegisterPanel(&registration, a_handle);
			if (result != Model::RegistrationResult::Success) {
				delete consumerPanel;
			}
			// Successful registrations intentionally retain their tiny callback
			// state for the process lifetime; SFSE does not hot-unload plugins.
			return result;
		} catch (const std::bad_alloc&) {
			return Model::RegistrationResult::OutOfMemory;
		}
	}
}
