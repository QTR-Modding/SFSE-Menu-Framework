#include "runtime/PanelRegistry.h"

#include "appearance/fonts/ConsumerFontScope.h"
#include "config/RootMenuConfig.h"
#include "runtime/ConsumerValidation.h"
#include "runtime/MenuPath.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace SFSEMenuFramework
{
	namespace
	{
		// Rename/delete behavior adapts SKSE Menu Framework 3 UI.cpp at
		// accepted base 8a366c4a3db7317655cec8379e48c43140c9fe7d (GPL-3.0). SFSE uses
		// immutable path-copy publication instead of mutating render-owned nodes.
		enum class MenuMutation
		{
			Rename,
			Delete
		};

		struct PanelRegistryState final
		{
			std::mutex                                  Mutex;
			std::atomic<PanelRegistry::MenuTreePointer> Roots;
			std::uint64_t                               NextIdentity{ 1 };
		};

		[[nodiscard]] PanelRegistryState* GetPanelRegistryState() noexcept
		{
			static auto* registry = new (std::nothrow) PanelRegistryState();
			return registry;
		}

		[[nodiscard]] PanelRegistry::MenuNodePointer CloneNode(
			const PanelRegistry::MenuNodePointer& a_node)
		{
			auto clone = std::make_shared<PanelRegistry::MenuNode>();
			clone->Name = a_node->Name;
			clone->Identity = a_node->Identity;
			clone->Panel.store(
				a_node->Panel.load(std::memory_order_acquire),
				std::memory_order_relaxed);
			clone->Children.store(
				a_node->Children.load(std::memory_order_acquire),
				std::memory_order_relaxed);
			return clone;
		}

		[[nodiscard]] PanelRegistry::MenuNodePointer FindRootByIdentity(
			const PanelRegistry::MenuTreePointer& a_roots,
			std::uint64_t                         a_identity)
		{
			if (!a_roots || a_identity == 0) {
				return nullptr;
			}
			const auto found = std::ranges::find_if(
				*a_roots,
				[&](const auto& a_root) {
					return a_root && a_root->Identity == a_identity;
				});
			return found != a_roots->end() ? *found : nullptr;
		}

		[[nodiscard]] bool MutateMenuTree(
			const PanelRegistry::MenuTreePointer& a_nodes,
			const MenuPath::Segments&              a_path,
			std::size_t                            a_pathIndex,
			MenuMutation                           a_mutation,
			const std::string&                     a_newName,
			PanelRegistry::MenuTreePointer&         a_updated)
		{
			if (!a_nodes) {
				return false;
			}

			const auto found = std::ranges::find_if(
				*a_nodes,
				[&](const auto& a_node) {
					return a_node && a_node->Name == a_path[a_pathIndex];
				});
			if (found == a_nodes->end()) {
				return false;
			}

			const auto nodeIndex = static_cast<std::size_t>(
				std::distance(a_nodes->begin(), found));
			const auto& node = *found;
			if (a_pathIndex + 1 < a_path.size()) {
				const auto children =
					node->Children.load(std::memory_order_acquire);
				PanelRegistry::MenuTreePointer updatedChildren;
				if (!MutateMenuTree(
						children,
						a_path,
						a_pathIndex + 1,
						a_mutation,
						a_newName,
						updatedChildren)) {
					return false;
				}
				if (updatedChildren == children) {
					a_updated = a_nodes;
					return true;
				}

				auto replacement = CloneNode(node);
				replacement->Children.store(
					std::move(updatedChildren), std::memory_order_relaxed);
				auto updated =
					std::make_shared<PanelRegistry::MenuTree>(*a_nodes);
				(*updated)[nodeIndex] = std::move(replacement);
				a_updated = std::move(updated);
				return true;
			}

			if (a_mutation == MenuMutation::Delete) {
				auto updated =
					std::make_shared<PanelRegistry::MenuTree>(*a_nodes);
				updated->erase(updated->begin() +
					static_cast<std::ptrdiff_t>(nodeIndex));
				a_updated = std::move(updated);
				return true;
			}

			if (node->Name == a_newName) {
				a_updated = a_nodes;
				return true;
			}
			if (std::ranges::any_of(
					*a_nodes,
					[&](const auto& a_sibling) {
						return a_sibling && a_sibling->Name == a_newName;
					})) {
				return false;
			}

			auto replacement = CloneNode(node);
			replacement->Name = a_newName;
			auto updated = std::make_shared<PanelRegistry::MenuTree>(*a_nodes);
			(*updated)[nodeIndex] = std::move(replacement);
			a_updated = std::move(updated);
			return true;
		}

		[[nodiscard]] bool AddToMenuTree(
			PanelRegistryState&                a_registry,
			const PanelRegistry::PanelPointer& a_panel,
			const MenuPath::Segments&          a_path)
		{
			auto* children = &a_registry.Roots;
			PanelRegistry::MenuNodePointer node;
			std::size_t firstMissing{};
			for (; firstMissing < a_path.size(); ++firstMissing) {
				const auto nodes = children->load(std::memory_order_acquire);
				node.reset();
				if (nodes) {
					const auto found = std::ranges::find_if(
						*nodes,
						[&](const auto& a_entry) {
							return a_entry &&
								a_entry->Name == a_path[firstMissing];
						});
					if (found != nodes->end()) {
						node = *found;
					}
				}
				if (!node) {
					break;
				}
				children = &node->Children;
			}

			if (firstMissing == a_path.size()) {
				// Match the source framework: the newest registration for an
				// existing path replaces the previous renderer.
				node->Panel.store(a_panel, std::memory_order_release);
				return true;
			}

			// Allocate the complete missing branch before publishing its root.
			// A failed allocation therefore cannot expose partial empty nodes.
			PanelRegistry::MenuNodePointer branch;
			for (auto index = a_path.size(); index-- > firstMissing;) {
				node = std::make_shared<PanelRegistry::MenuNode>();
				node->Name = a_path[index];
				node->Identity = a_registry.NextIdentity++;
				if (branch) {
					auto list = std::make_shared<PanelRegistry::MenuTree>();
					list->push_back(std::move(branch));
					node->Children.store(
						std::move(list), std::memory_order_relaxed);
				} else {
					node->Panel.store(a_panel, std::memory_order_relaxed);
				}
				branch = std::move(node);
			}

			const auto current = children->load(std::memory_order_acquire);
			auto next = current ?
			                std::make_shared<PanelRegistry::MenuTree>(*current) :
			                std::make_shared<PanelRegistry::MenuTree>();
			next->push_back(std::move(branch));
			children->store(std::move(next), std::memory_order_release);
			return true;
		}

		[[nodiscard]] bool Mutate(
			MenuMutation a_mutation,
			std::string_view a_path,
			std::string_view a_newName = {}) noexcept
		{
			try {
				auto parsedPath = MenuPath::Parse(a_path);
				if (!parsedPath) {
					return false;
				}

				std::string parsedNewName;
				if (a_mutation == MenuMutation::Rename) {
					auto parsed = MenuPath::ParseSegment(a_newName);
					if (!parsed) {
						return false;
					}
					parsedNewName = std::move(*parsed);
				}

				auto* registry = GetPanelRegistryState();
				if (!registry) {
					return false;
				}
				std::scoped_lock lock{ registry->Mutex };
				const auto current =
					registry->Roots.load(std::memory_order_acquire);
				PanelRegistry::MenuTreePointer updated;
				if (!MutateMenuTree(
						current,
						*parsedPath,
						0,
						a_mutation,
						parsedNewName,
						updated)) {
					return false;
				}
				if (updated == current) {
					return true;
				}

				registry->Roots.store(updated, std::memory_order_release);
				if (parsedPath->size() == 1) {
					const bool saved = a_mutation == MenuMutation::Rename ?
						RootMenuConfig::RenameMenu(
							parsedPath->front(), parsedNewName) :
						RootMenuConfig::RemoveMenu(parsedPath->front());
					if (!saved) {
						logger::warn(
							"Applied root-menu mutation for '{}' but could not save its favorite/archive state",
							a_path);
					}
				}
				return true;
			} catch (const std::bad_alloc&) {
				return false;
			} catch (const std::system_error&) {
				return false;
			}
		}
	}

	bool PanelRegistry::RegisterDirect(
		std::string_view      a_path,
		DirectRenderFunction a_render) noexcept
	{
		if (!a_render || !Detail::IsExecutableImageFunction(a_render)) {
			return false;
		}

		try {
			auto parsedPath = MenuPath::Parse(a_path);
			if (!parsedPath) {
				return false;
			}
			auto panel = std::make_shared<Panel>();
			panel->Render = a_render;

			auto* registry = GetPanelRegistryState();
			if (!registry) {
				return false;
			}
			std::scoped_lock lock{ registry->Mutex };
			return AddToMenuTree(*registry, panel, *parsedPath);
		} catch (const std::bad_alloc&) {
			return false;
		} catch (const std::system_error&) {
			return false;
		}
	}

	bool PanelRegistry::Rename(
		std::string_view a_path,
		std::string_view a_newName) noexcept
	{
		return Mutate(MenuMutation::Rename, a_path, a_newName);
	}

	bool PanelRegistry::Delete(std::string_view a_path) noexcept
	{
		return Mutate(MenuMutation::Delete, a_path);
	}

	PanelRegistry::MenuTreePointer PanelRegistry::GetMenuTree() noexcept
	{
		const auto* registry = GetPanelRegistryState();
		return registry ?
			registry->Roots.load(std::memory_order_acquire) : nullptr;
	}

	std::optional<PanelRegistry::RootState> PanelRegistry::GetRootState(
		std::uint64_t a_identity) noexcept
	{
		try {
			auto* registry = GetPanelRegistryState();
			if (!registry) {
				return std::nullopt;
			}
			std::scoped_lock lock{ registry->Mutex };
			const auto root = FindRootByIdentity(
				registry->Roots.load(std::memory_order_acquire),
				a_identity);
			if (!root) {
				return std::nullopt;
			}
			return RootState{
				RootMenuConfig::IsFavorite(root->Name),
				RootMenuConfig::IsArchived(root->Name)
			};
		} catch (const std::system_error&) {
			return std::nullopt;
		}
	}

	bool PanelRegistry::SetRootFavorite(
		std::uint64_t a_identity,
		bool          a_favorite) noexcept
	{
		try {
			auto* registry = GetPanelRegistryState();
			if (!registry) {
				return false;
			}
			std::scoped_lock lock{ registry->Mutex };
			const auto root = FindRootByIdentity(
				registry->Roots.load(std::memory_order_acquire),
				a_identity);
			return root &&
				RootMenuConfig::SetFavorite(root->Name, a_favorite);
		} catch (const std::system_error&) {
			return false;
		}
	}

	bool PanelRegistry::SetRootArchived(
		std::uint64_t a_identity,
		bool          a_archived) noexcept
	{
		try {
			auto* registry = GetPanelRegistryState();
			if (!registry) {
				return false;
			}
			std::scoped_lock lock{ registry->Mutex };
			const auto root = FindRootByIdentity(
				registry->Roots.load(std::memory_order_acquire),
				a_identity);
			return root &&
				RootMenuConfig::SetArchived(root->Name, a_archived);
		} catch (const std::system_error&) {
			return false;
		}
	}

	void PanelRegistry::Render(const PanelPointer& a_panel)
	{
		if (!a_panel || !a_panel->Render) {
			return;
		}
		ConsumerFontScope::CallbackScope callbackScope{ "panel" };
		a_panel->Render();
	}
}
