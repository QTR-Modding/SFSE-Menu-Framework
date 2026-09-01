#pragma once
#include <SFSEMenuFramework/API.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SFSEMenuFramework
{
	namespace Detail
	{
		struct EventSnapshot;
		[[nodiscard]] bool HasMatchingImGuiLayout(const Model::ImGuiLayout& a_layout) noexcept;
		[[nodiscard]] bool IsExecutableImageAddress(
			const void* a_address, void** a_ownerModule = nullptr) noexcept;
		template <class Function>
		[[nodiscard]] bool IsExecutableImageFunction(
			Function a_function, void** a_ownerModule = nullptr) noexcept
		{
			static_assert(sizeof(a_function) == sizeof(const void*));
			return a_function && IsExecutableImageAddress(
				std::bit_cast<const void*>(a_function), a_ownerModule);
		}
	}

	class EventManager final
	{
	public:
		using Snapshot = std::shared_ptr<const Detail::EventSnapshot>;
		[[nodiscard]] static Model::RegistrationResult Register(
			const Model::EventRegistration* a_registration, Model::EventHandle* a_handle) noexcept;
		static void Unregister(Model::EventHandle a_handle) noexcept;
		[[nodiscard]] static bool SetMainWindowState(
			std::atomic<bool>& a_state, bool a_open,
			bool               a_emergencyClose = false) noexcept;
		[[nodiscard]] static bool BeginFrame(Snapshot& a_snapshot) noexcept;
		static void Dispatch(Model::EventType a_type, const Snapshot& a_snapshot) noexcept;
	};

	class PanelRegistry final
	{
	public:
		struct Panel final
		{
			void*                      OwnerModule{ nullptr };
			std::string                Id;
			std::string                Section;
			std::string                Title;
			Model::PanelRenderFunction Render{ nullptr };
			void*                      UserData{ nullptr };
			std::atomic<bool>          Enabled{ true };
		};
		using PanelPointer = std::shared_ptr<Panel>;
		struct MenuNode final
		{
			using List = std::vector<std::shared_ptr<MenuNode>>;
			using ListPointer = std::shared_ptr<const List>;
			std::string               Name;
			std::string               FullPath;
			std::atomic<PanelPointer>  Panel;
			std::atomic<ListPointer>   Children;
		};
		using MenuNodePointer = std::shared_ptr<MenuNode>;
		using MenuTree = MenuNode::List;
		using MenuTreePointer = MenuNode::ListPointer;
		[[nodiscard]] static Model::RegistrationResult Register(
			const Model::PanelRegistration* a_registration, Model::PanelHandle* a_handle) noexcept;
		[[nodiscard]] static MenuTreePointer GetMenuTree() noexcept;
		static void Render(
			const PanelPointer& a_panel, const Model::RenderContext& a_context);
	};

	class WindowInterface final : public Model::WindowInterface
	{
	public:
		std::atomic<bool> PauseGame{ true };
		std::atomic<bool> BlurBackground{ true };
	};

	using WindowRenderFunction = void(__stdcall*)(const Model::RenderContext&);

	class WindowManager final
	{
	public:
		static WindowInterface* AddWindow(WindowRenderFunction a_renderFunction);
		[[nodiscard]] static Model::RegistrationResult RegisterWindow(
			const Model::WindowRegistration* a_registration,
			Model::WindowInterface** a_window) noexcept;
		[[nodiscard]] static std::uint64_t RenderOpenWindows(const Model::RenderContext& a_context);
		static bool SetMainWindow(WindowInterface* a_window) noexcept;
		[[nodiscard]] static WindowInterface* GetMainWindow() noexcept;
		static void SetMainWindowRenderEnabled(bool a_enabled) noexcept;
		[[nodiscard]] static bool SetMainWindowOpen(bool a_open) noexcept;
		[[nodiscard]] static bool IsAnyWindowOpen() noexcept;
		[[nodiscard]] static bool IsAnyBlockingWindowOpened() noexcept;
		[[nodiscard]] static bool ShouldPauseGame() noexcept;
		[[nodiscard]] static bool ShouldBlurBackground() noexcept;
		static void CloseAllBlockingWindows() noexcept;
		static void SetHotkeyEnabled(bool a_enabled) noexcept;
		[[nodiscard]] static bool IsHotkeyEnabled() noexcept;
		[[nodiscard]] static std::uint64_t GetBlockingWindowOpenGeneration() noexcept;
		[[nodiscard]] static bool IsBlockingWindowOpenGeneration(std::uint64_t a_generation) noexcept;
		[[nodiscard]] static std::uint64_t GetMainWindowSessionGeneration() noexcept;
	};
}
