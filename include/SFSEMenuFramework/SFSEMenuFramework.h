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
		struct ConsumerCallback final
		{
			RenderFunction Render{ nullptr };
		};

		struct HudConsumerCallback final
		{
			Model::HudElementCallback Render{ nullptr };
		};

		inline thread_local std::string currentSection;

#if defined(IMGUI_DISABLE) || defined(IMGUI_DISABLE_OBSOLETE_FUNCTIONS) || \
	defined(IMGUI_DISABLE_OBSOLETE_KEYIO) || defined(IMGUI_DISABLE_DEBUG_TOOLS) || \
	defined(IMGUI_DISABLE_DEFAULT_ALLOCATORS) || \
	defined(IMGUI_USE_BGRA_PACKED_COLOR) || defined(IMGUI_USE_WCHAR32) || \
	defined(IMGUI_OVERRIDE_DRAWVERT_STRUCT_LAYOUT) || defined(ImTextureID) || \
	defined(ImDrawIdx) || defined(ImDrawCallback) || defined(IMGUI_USER_CONFIG) || \
	defined(IMGUI_INCLUDE_IMGUI_USER_H) || defined(IMGUI_ENABLE_FREETYPE) || \
	defined(IMGUI_ENABLE_FREETYPE_LUNASVG)
#error SFSE Menu Framework consumers require the pinned default ImGui configuration
#endif

		[[nodiscard]] inline Model::QueryInterfaceFunction GetQueryInterface() noexcept
		{
			if (const auto module = ::GetModuleHandleW(L"SFSEMenuFramework.dll");
				module) {
				const auto procedure =
					::GetProcAddress(module, "SFSEMenuFramework_QueryInterface");
				static_assert(sizeof(procedure) == sizeof(Model::QueryInterfaceFunction));
				return procedure ?
					std::bit_cast<Model::QueryInterfaceFunction>(procedure) : nullptr;
			} else {
				return nullptr;
			}
		}

		template <class Interface, std::uint32_t Version, auto... RequiredFunctions>
		[[nodiscard]] inline const Interface* RequestValidatedInterface() noexcept
		{
			if (const auto query = GetQueryInterface(); !query) {
				return nullptr;
			} else {
				const auto* result = reinterpret_cast<const Interface*>(query(Version));
				return result && result->StructureSize >= sizeof(Interface) &&
					result->Version == Version &&
					((result->*RequiredFunctions) && ...) ? result : nullptr;
			}
		}

		[[nodiscard]] inline const Model::Interface* RequestInterface() noexcept
		{
			return RequestValidatedInterface<Model::Interface,
				Model::INTERFACE_VERSION,
				&Model::Interface::RegisterPanel>();
		}

		[[nodiscard]] inline const Model::InterfaceV2* RequestInterfaceV2() noexcept
		{
			return RequestValidatedInterface<Model::InterfaceV2,
				Model::INTERFACE_VERSION_2,
				&Model::InterfaceV2::RegisterPanel,
				&Model::InterfaceV2::RegisterWindow,
				&Model::InterfaceV2::GetMainWindow,
				&Model::InterfaceV2::IsAnyBlockingWindowOpened,
				&Model::InterfaceV2::SetHotkeyEnabled,
				&Model::InterfaceV2::IsHotkeyEnabled>();
		}

		[[nodiscard]] inline const Model::InterfaceV3* RequestInterfaceV3() noexcept
		{
			return RequestValidatedInterface<Model::InterfaceV3,
				Model::INTERFACE_VERSION_3,
				&Model::InterfaceV3::RegisterPanel,
				&Model::InterfaceV3::RegisterWindow,
				&Model::InterfaceV3::GetMainWindow,
				&Model::InterfaceV3::IsAnyBlockingWindowOpened,
				&Model::InterfaceV3::SetHotkeyEnabled,
				&Model::InterfaceV3::IsHotkeyEnabled,
				&Model::InterfaceV3::RegisterEvent,
				&Model::InterfaceV3::UnregisterEvent>();
		}

		[[nodiscard]] inline const Model::InterfaceV4* RequestInterfaceV4() noexcept
		{
			return RequestValidatedInterface<Model::InterfaceV4,
				Model::INTERFACE_VERSION_4,
				&Model::InterfaceV4::RegisterPanel,
				&Model::InterfaceV4::RegisterWindow,
				&Model::InterfaceV4::GetMainWindow,
				&Model::InterfaceV4::IsAnyBlockingWindowOpened,
				&Model::InterfaceV4::SetHotkeyEnabled,
				&Model::InterfaceV4::IsHotkeyEnabled,
				&Model::InterfaceV4::RegisterEvent,
				&Model::InterfaceV4::UnregisterEvent,
				&Model::InterfaceV4::RegisterInputEvent,
				&Model::InterfaceV4::UnregisterInputEvent,
				&Model::InterfaceV4::RegisterHudElement,
				&Model::InterfaceV4::UnregisterHudElement>();
		}

		[[nodiscard]] inline const Model::InterfaceV5* RequestInterfaceV5() noexcept
		{
			return RequestValidatedInterface<Model::InterfaceV5,
				Model::INTERFACE_VERSION_5,
				&Model::InterfaceV5::RegisterPanel,
				&Model::InterfaceV5::RegisterWindow,
				&Model::InterfaceV5::GetMainWindow,
				&Model::InterfaceV5::IsAnyBlockingWindowOpened,
				&Model::InterfaceV5::SetHotkeyEnabled,
				&Model::InterfaceV5::IsHotkeyEnabled,
				&Model::InterfaceV5::RegisterEvent,
				&Model::InterfaceV5::UnregisterEvent,
				&Model::InterfaceV5::RegisterInputEvent,
				&Model::InterfaceV5::UnregisterInputEvent,
				&Model::InterfaceV5::RegisterHudElement,
				&Model::InterfaceV5::UnregisterHudElement,
				&Model::InterfaceV5::PushFont,
				&Model::InterfaceV5::PopFont>();
		}

		[[nodiscard]] inline Model::ImGuiLayout GetImGuiLayout() noexcept
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

		template <class Consumer>
		[[nodiscard]] inline bool InvokeConsumer(
			const Model::RenderContext* a_context, void* a_userData) noexcept
		{
			if (!a_context ||
				a_context->StructureSize < sizeof(Model::RenderContext) ||
				a_context->InterfaceVersion != Model::INTERFACE_VERSION ||
				!a_context->ImGuiContext ||
				!a_context->Allocate ||
				!a_context->Free ||
				!a_userData) {
				return false;
			}
			const auto render = static_cast<Consumer*>(a_userData)->Render;
			if (!render) {
				return false;
			}

			static_assert(std::is_same_v<Model::ImGuiAllocateFunction, ImGuiMemAllocFunc>);
			static_assert(std::is_same_v<Model::ImGuiFreeFunction, ImGuiMemFreeFunc>);

			auto* const previousContext = ImGui::GetCurrentContext();
			ImGuiMemAllocFunc previousAllocate{};
			ImGuiMemFreeFunc previousFree{};
			void* previousAllocatorUserData{};
			ImGui::GetAllocatorFunctions(&previousAllocate, &previousFree,
				&previousAllocatorUserData);
			ImGui::SetAllocatorFunctions(a_context->Allocate, a_context->Free,
				a_context->AllocatorUserData);
			ImGui::SetCurrentContext(
				static_cast<ImGuiContext*>(a_context->ImGuiContext));
			render();
			ImGui::SetCurrentContext(previousContext);
			ImGui::SetAllocatorFunctions(previousAllocate, previousFree,
				previousAllocatorUserData);
			return true;
		}

		[[nodiscard]] inline Model::PanelRenderResult __stdcall RenderPanel(
			const Model::RenderContext* a_context, void* a_userData) noexcept
		{
			return InvokeConsumer<ConsumerCallback>(a_context, a_userData) ?
				Model::PanelRenderResult::Continue :
				Model::PanelRenderResult::Failed;
		}

		inline void __stdcall RenderWindow(
			const Model::RenderContext* a_context, void* a_userData) noexcept
		{
			static_cast<void>(
				InvokeConsumer<ConsumerCallback>(a_context, a_userData));
		}

		inline void __stdcall RenderHudElement(
			const Model::RenderContext* a_context, void* a_userData) noexcept
		{
			static_cast<void>(
				InvokeConsumer<HudConsumerCallback>(a_context, a_userData));
		}

		[[nodiscard]] inline bool IsValidText(
			std::string_view a_text, std::uint32_t a_maximumLength) noexcept
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

	namespace Model
	{
		class Event;
		class InputEvent;
		class HudElement;
	}

	[[nodiscard]] Model::Event* AddEvent(
		Model::EventCallback a_callback,
		float                a_priority = 0.0F) noexcept;
	[[nodiscard]] Model::InputEvent* AddInputEvent(
		Model::InputEventCallback a_callback) noexcept;
	[[nodiscard]] Model::HudElement* AddHudElement(
		Model::HudElementCallback a_callback) noexcept;

	namespace Model
	{
		class Event final
		{
		public:
			Event(const Event&) = delete;
			Event(Event&&) = delete;
			Event& operator=(const Event&) = delete;
			Event& operator=(Event&&) = delete;

			~Event() noexcept {
				if (Handle != 0) {
					if (const auto* api = Detail::RequestInterfaceV3()) {
						api->UnregisterEvent(Handle);
					}
					Handle = 0;
				}
			}

		private:
			friend Event* ::SFSEMenuFramework::AddEvent(
				EventCallback,
				float) noexcept;

			explicit Event(EventHandle a_handle) noexcept : Handle(a_handle) {}

			EventHandle Handle{ 0 };
		};

		class InputEvent final
		{
		public:
			InputEvent(const InputEvent&) = delete;
			InputEvent(InputEvent&&) = delete;
			InputEvent& operator=(const InputEvent&) = delete;
			InputEvent& operator=(InputEvent&&) = delete;

			~InputEvent() noexcept
			{
				if (Handle != 0) {
					if (const auto* api = Detail::RequestInterfaceV4()) {
						api->UnregisterInputEvent(Handle);
					}
					Handle = 0;
				}
			}

		private:
			friend InputEvent* ::SFSEMenuFramework::AddInputEvent(
				InputEventCallback) noexcept;

			explicit InputEvent(InputEventHandle a_handle) noexcept :
				Handle(a_handle)
			{}

			InputEventHandle Handle{ 0 };
		};

		class HudElement final
		{
		public:
			HudElement(const HudElement&) = delete;
			HudElement(HudElement&&) = delete;
			HudElement& operator=(const HudElement&) = delete;
			HudElement& operator=(HudElement&&) = delete;

			~HudElement() noexcept
			{
				if (Handle != 0) {
					if (const auto* api = Detail::RequestInterfaceV4()) {
						api->UnregisterHudElement(Handle);
					}
					Handle = 0;
				}
				delete CallbackState;
				CallbackState = nullptr;
			}

		private:
			friend HudElement* ::SFSEMenuFramework::AddHudElement(
				HudElementCallback) noexcept;

			HudElement(
				HudElementHandle                            a_handle,
				::SFSEMenuFramework::Detail::HudConsumerCallback* a_callbackState) noexcept :
				Handle(a_handle),
				CallbackState(a_callbackState)
			{}

			HudElementHandle Handle{ 0 };
			::SFSEMenuFramework::Detail::HudConsumerCallback*
				CallbackState{ nullptr };
		};
	}

	[[nodiscard]] inline bool IsInstalled() noexcept
	{
		return Detail::RequestInterface() != nullptr;
	}

	[[nodiscard]] inline bool IsEventAPIAvailable() noexcept
	{
		return Detail::RequestInterfaceV3() != nullptr;
	}

	[[nodiscard]] inline bool IsInputEventAPIAvailable() noexcept
	{
		return Detail::RequestInterfaceV4() != nullptr;
	}

	[[nodiscard]] inline bool IsHudElementAPIAvailable() noexcept
	{
		return Detail::RequestInterfaceV4() != nullptr;
	}

	[[nodiscard]] inline bool IsFontAPIAvailable() noexcept
	{
		return Detail::RequestInterfaceV5() != nullptr;
	}

	[[nodiscard]] inline bool PushFont(std::string_view a_name) noexcept
	{
		if (!Detail::IsValidText(a_name, Model::MAXIMUM_FONT_NAME_LENGTH)) {
			return false;
		}
		const auto* api = Detail::RequestInterfaceV5();
		if (!api) {
			return false;
		}
		const auto name = Detail::ToModelString(a_name);
		return api->PushFont(&name);
	}

	[[nodiscard]] inline bool PopFont() noexcept
	{
		const auto* api = Detail::RequestInterfaceV5();
		return api && api->PopFont();
	}

	class ScopedFont final
	{
	public:
		explicit ScopedFont(std::string_view a_name) noexcept :
			Pushed(PushFont(a_name))
		{}
		~ScopedFont() noexcept
		{
			if (Pushed) {
				static_cast<void>(PopFont());
			}
		}

		ScopedFont(const ScopedFont&) = delete;
		ScopedFont(ScopedFont&&) = delete;
		ScopedFont& operator=(const ScopedFont&) = delete;
		ScopedFont& operator=(ScopedFont&&) = delete;

		[[nodiscard]] explicit operator bool() const noexcept { return Pushed; }

	private:
		bool Pushed{};
	};

	namespace FontAwesome
	{
		inline constexpr std::string_view SolidFont{ "fa-solid-900.ttf" };
		inline constexpr std::string_view RegularFont{ "fa-regular-400.ttf" };
		inline constexpr std::string_view BrandsFont{ "fa-brands-400.ttf" };

		[[nodiscard]] inline bool PushSolid() noexcept
		{
			return SFSEMenuFramework::PushFont(SolidFont);
		}
		[[nodiscard]] inline bool PushRegular() noexcept
		{
			return SFSEMenuFramework::PushFont(RegularFont);
		}
		[[nodiscard]] inline bool PushBrands() noexcept
		{
			return SFSEMenuFramework::PushFont(BrandsFont);
		}
		[[nodiscard]] inline bool Pop() noexcept
		{
			return SFSEMenuFramework::PopFont();
		}

		// The helper name and UTF-8 result behavior adapt SKSE Menu Framework 3
		// at commit 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// This port replaces codecvt and returns an empty string for invalid scalars.
		[[nodiscard]] inline std::string UnicodeToUtf8(
			unsigned int a_codepoint)
		{
			if (a_codepoint > 0x10FFFFU ||
				(a_codepoint >= 0xD800U && a_codepoint <= 0xDFFFU)) {
				return {};
			}

			std::string result;
			if (a_codepoint <= 0x7FU) {
				result.push_back(static_cast<char>(a_codepoint));
			} else if (a_codepoint <= 0x7FFU) {
				result.push_back(static_cast<char>(0xC0U | (a_codepoint >> 6U)));
				result.push_back(static_cast<char>(0x80U | (a_codepoint & 0x3FU)));
			} else if (a_codepoint <= 0xFFFFU) {
				result.push_back(static_cast<char>(0xE0U | (a_codepoint >> 12U)));
				result.push_back(static_cast<char>(0x80U | ((a_codepoint >> 6U) & 0x3FU)));
				result.push_back(static_cast<char>(0x80U | (a_codepoint & 0x3FU)));
			} else {
				result.push_back(static_cast<char>(0xF0U | (a_codepoint >> 18U)));
				result.push_back(static_cast<char>(0x80U | ((a_codepoint >> 12U) & 0x3FU)));
				result.push_back(static_cast<char>(0x80U | ((a_codepoint >> 6U) & 0x3FU)));
				result.push_back(static_cast<char>(0x80U | (a_codepoint & 0x3FU)));
			}
			return result;
		}
	}

	[[nodiscard]] inline bool SetSection(std::string_view a_section)
	{
		if (!Detail::IsValidText(a_section, Model::MAXIMUM_PANEL_TEXT_LENGTH)) {
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
			!Detail::IsValidText(Detail::currentSection,
				Model::MAXIMUM_PANEL_TEXT_LENGTH) ||
			!Detail::IsValidText(a_title, Model::MAXIMUM_PANEL_TEXT_LENGTH)) {
			return Model::RegistrationResult::InvalidArgument;
		}

		const auto* api = Detail::RequestInterface();
		if (!api) {
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

			auto* consumerPanel = new (std::nothrow)
				Detail::ConsumerCallback{ a_renderFunction };
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

			const auto result = api->RegisterPanel(&registration, a_handle);
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

	[[nodiscard]] inline Model::WindowInterface* AddWindow(
		RenderFunction a_renderFunction,
		bool           a_doesWindowPauseGame = true) noexcept
	{
		if (!a_renderFunction) {
			return nullptr;
		}

		const auto* api = Detail::RequestInterfaceV2();
		if (!api) {
			return nullptr;
		}

		auto* consumerWindow = new (std::nothrow)
			Detail::ConsumerCallback{ a_renderFunction };
		if (!consumerWindow) {
			return nullptr;
		}

		const Model::WindowRegistration registration{
			.StructureSize = sizeof(Model::WindowRegistration),
			.InterfaceVersion = Model::INTERFACE_VERSION_2,
			.ImGui = Detail::GetImGuiLayout(),
			.Render = &Detail::RenderWindow,
			.UserData = consumerWindow,
			.BlockUserInput = static_cast<std::uint8_t>(a_doesWindowPauseGame)
		};
		Model::WindowInterface* window{};
		const auto result = api->RegisterWindow(&registration, &window);
		if (result != Model::RegistrationResult::Success) {
			delete consumerWindow;
			return nullptr;
		}

		// Successful registrations intentionally retain their tiny callback
		// state for the process lifetime; SFSE does not hot-unload plugins.
		return window;
	}

	[[nodiscard]] inline Model::Event* AddEvent(
		Model::EventCallback a_callback,
		float                a_priority) noexcept
	{
		if (!a_callback) {
			return nullptr;
		}

		const auto* api = Detail::RequestInterfaceV3();
		if (!api) {
			return nullptr;
		}

		const Model::EventRegistration registration{
			.StructureSize = sizeof(Model::EventRegistration),
			.InterfaceVersion = Model::INTERFACE_VERSION_3,
			.Callback = a_callback,
			.Priority = a_priority
		};
		Model::EventHandle handle{};
		const auto result = api->RegisterEvent(&registration, &handle);
		if (result != Model::RegistrationResult::Success || handle == 0) {
			return nullptr;
		}

		auto* event = new (std::nothrow) Model::Event(handle);
		if (!event) {
			api->UnregisterEvent(handle);
		}
		return event;
	}

	[[nodiscard]] inline Model::InputEvent* AddInputEvent(
		Model::InputEventCallback a_callback) noexcept
	{
		if (!a_callback) {
			return nullptr;
		}

		const auto* api = Detail::RequestInterfaceV4();
		if (!api) {
			return nullptr;
		}

		const Model::InputEventRegistration registration{
			.StructureSize = sizeof(Model::InputEventRegistration),
			.InterfaceVersion = Model::INTERFACE_VERSION_4,
			.Callback = a_callback
		};
		Model::InputEventHandle handle{};
		const auto result = api->RegisterInputEvent(&registration, &handle);
		if (result != Model::RegistrationResult::Success || handle == 0) {
			return nullptr;
		}

		auto* inputEvent = new (std::nothrow) Model::InputEvent(handle);
		if (!inputEvent) {
			api->UnregisterInputEvent(handle);
		}
		return inputEvent;
	}

	[[nodiscard]] inline Model::HudElement* AddHudElement(
		Model::HudElementCallback a_callback) noexcept
	{
		if (!a_callback) {
			return nullptr;
		}

		const auto* api = Detail::RequestInterfaceV4();
		if (!api) {
			return nullptr;
		}

		auto* callbackState = new (std::nothrow)
			Detail::HudConsumerCallback{ a_callback };
		if (!callbackState) {
			return nullptr;
		}

		const Model::HudElementRegistration registration{
			.StructureSize = sizeof(Model::HudElementRegistration),
			.InterfaceVersion = Model::INTERFACE_VERSION_4,
			.ImGui = Detail::GetImGuiLayout(),
			.Render = &Detail::RenderHudElement,
			.UserData = callbackState
		};
		Model::HudElementHandle handle{};
		const auto result = api->RegisterHudElement(&registration, &handle);
		if (result != Model::RegistrationResult::Success || handle == 0) {
			delete callbackState;
			return nullptr;
		}

		auto* hudElement =
			new (std::nothrow) Model::HudElement(handle, callbackState);
		if (!hudElement) {
			api->UnregisterHudElement(handle);
			delete callbackState;
		}
		return hudElement;
	}

	[[nodiscard]] inline Model::WindowInterface* GetMainWindow() noexcept
	{
		const auto* api = Detail::RequestInterfaceV2();
		return api ? api->GetMainWindow() : nullptr;
	}

	[[nodiscard]] inline bool IsAnyBlockingWindowOpened() noexcept
	{
		const auto* api = Detail::RequestInterfaceV2();
		return api ? api->IsAnyBlockingWindowOpened() : false;
	}

	inline void SetHotkeyEnabled(bool a_enabled) noexcept
	{
		if (const auto* api = Detail::RequestInterfaceV2()) {
			api->SetHotkeyEnabled(a_enabled);
		}
	}

	[[nodiscard]] inline bool IsHotkeyEnabled() noexcept
	{
		const auto* api = Detail::RequestInterfaceV2();
		return api ? api->IsHotkeyEnabled() : false;
	}
}
