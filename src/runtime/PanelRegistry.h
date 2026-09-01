#pragma once

#include <SFSEMenuFramework/API.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace SFSEMenuFramework
{
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

			std::string              Name;
			std::string              FullPath;
			std::atomic<PanelPointer> Panel;
			std::atomic<ListPointer>  Children;
		};
		using MenuNodePointer = std::shared_ptr<MenuNode>;
		using MenuTree = MenuNode::List;
		using MenuTreePointer = MenuNode::ListPointer;

		[[nodiscard]] static Model::RegistrationResult Register(
			const Model::PanelRegistration* a_registration,
			Model::PanelHandle*              a_handle) noexcept;
		[[nodiscard]] static MenuTreePointer GetMenuTree() noexcept;
		static void Render(
			const PanelPointer& a_panel,
			const Model::RenderContext& a_context);
	};
}
