#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
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
			std::uint64_t            Identity{};
			std::atomic<PanelPointer> Panel;
			std::atomic<ListPointer>  Children;
		};
		using MenuNodePointer = std::shared_ptr<MenuNode>;
		using MenuTree = MenuNode::List;
		using MenuTreePointer = MenuNode::ListPointer;
		struct RootState final
		{
			bool Favorite{};
			bool Archived{};
		};

		[[nodiscard]] static bool RegisterDirect(
			std::string_view a_path,
			DirectRenderFunction a_render) noexcept;
		[[nodiscard]] static bool Rename(
			std::string_view a_path,
			std::string_view a_newName) noexcept;
		[[nodiscard]] static bool Delete(std::string_view a_path) noexcept;
		[[nodiscard]] static MenuTreePointer GetMenuTree() noexcept;
		[[nodiscard]] static std::optional<RootState> GetRootState(
			std::uint64_t a_identity) noexcept;
		[[nodiscard]] static bool SetRootFavorite(
			std::uint64_t a_identity, bool a_favorite) noexcept;
		[[nodiscard]] static bool SetRootArchived(
			std::uint64_t a_identity, bool a_archived) noexcept;
		static void Render(const PanelPointer& a_panel);
	};
}
