#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace SFSEMenuFramework
{
	class PanelRegistry final
	{
	public:
		using DirectRenderFunction = void(__stdcall*)();

		struct Panel final
		{
			DirectRenderFunction Render{ nullptr };
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

		[[nodiscard]] static bool RegisterDirect(
			std::string_view a_path,
			DirectRenderFunction a_render) noexcept;
		[[nodiscard]] static MenuTreePointer GetMenuTree() noexcept;
		static void Render(const PanelPointer& a_panel);
	};
}
