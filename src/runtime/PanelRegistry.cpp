#include "runtime/PanelRegistry.h"
#include "appearance/fonts/ConsumerFontScope.h"
#include "runtime/ConsumerValidation.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace SFSEMenuFramework
{
	namespace
	{
		constexpr std::size_t maximumPanelPathLength = 1024;
		struct PanelRegistryState final
		{
			std::mutex                                  Mutex;
			std::atomic<PanelRegistry::MenuTreePointer> Roots;
		};
		[[nodiscard]] PanelRegistryState* GetPanelRegistryState() noexcept
		{
			static auto* registry = new (std::nothrow) PanelRegistryState();
			return registry;
		}
		[[nodiscard]] bool IsValidPath(std::string_view a_path) noexcept
		{
			return !a_path.empty() &&
				a_path.size() <= maximumPanelPathLength &&
				a_path.front() != '/' && a_path.back() != '/' &&
				a_path.find("//") == std::string_view::npos;
		}
		[[nodiscard]] bool AddToMenuTree(
			PanelRegistryState&                a_registry,
			const PanelRegistry::PanelPointer& a_panel,
			std::string_view                   a_path)
		{
			std::vector<std::string_view> parts;
			for (std::size_t begin = 0; begin < a_path.size();) {
				const auto slash = a_path.find('/', begin);
				parts.push_back(a_path.substr(
					begin,
					slash == std::string::npos ? slash : slash - begin));
				if (slash == std::string::npos) {
					break;
				}
				begin = slash + 1;
			}
			auto* children = &a_registry.Roots;
			PanelRegistry::MenuNodePointer node;
			std::size_t firstMissing{};
			for (; firstMissing < parts.size(); ++firstMissing) {
				const auto nodes = children->load(std::memory_order_acquire);
				const auto found = nodes ? std::ranges::find_if(
					*nodes, [&](const auto& entry) {
						return entry && entry->Name == parts[firstMissing];
					}) : PanelRegistry::MenuTree::const_iterator{};
				node = nodes && found != nodes->end() ? *found : nullptr;
				if (!node) {
					break;
				}
				children = &node->Children;
			}
			if (firstMissing == parts.size()) {
				if (node->Panel.load(std::memory_order_acquire)) {
					return false;
				}
				node->Panel.store(a_panel, std::memory_order_release);
				return true;
			}
			// Allocate the complete missing branch before publishing its root.
			// A failed allocation therefore cannot expose partial empty nodes.
			PanelRegistry::MenuNodePointer branch;
			for (auto index = parts.size(); index-- > firstMissing;) {
				node = std::make_shared<PanelRegistry::MenuNode>();
				node->Name = parts[index];
				const auto prefixLength = static_cast<std::size_t>(
					parts[index].data() - a_path.data()) + parts[index].size();
				node->FullPath.assign(a_path.data(), prefixLength);
				if (branch) {
					auto list = std::make_shared<PanelRegistry::MenuTree>();
					list->push_back(std::move(branch));
					node->Children.store(std::move(list), std::memory_order_relaxed);
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
	}

	bool PanelRegistry::RegisterDirect(
		std::string_view      a_path,
		DirectRenderFunction a_render) noexcept
	{
		if (!IsValidPath(a_path) || !a_render) {
			return false;
		}
		if (!Detail::IsExecutableImageFunction(a_render)) {
			return false;
		}
		try {
			auto panel = std::make_shared<Panel>();
			panel->Render = a_render;

			auto* registry = GetPanelRegistryState();
			if (!registry) {
				return false;
			}
			std::scoped_lock lock{ registry->Mutex };
			if (!AddToMenuTree(*registry, panel, a_path)) {
				logger::warn("Rejected duplicate panel path '{}'", a_path);
				return false;
			}
			return true;
		} catch (const std::bad_alloc&) {
			return false;
		} catch (const std::system_error&) {
			return false;
		}
	}

	PanelRegistry::MenuTreePointer PanelRegistry::GetMenuTree() noexcept
	{
		const auto* registry = GetPanelRegistryState();
		return registry ? registry->Roots.load(std::memory_order_acquire) : nullptr;
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
