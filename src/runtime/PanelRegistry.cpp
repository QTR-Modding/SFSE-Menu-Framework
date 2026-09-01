#include "runtime/PanelRegistry.h"
#include "runtime/ConsumerValidation.h"

#include <algorithm>
#include <cstring>
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
		constexpr std::size_t maximumPanelCount = 1024;
		struct PanelRegistryState final
		{
			std::mutex                                 Mutex;
			std::vector<PanelRegistry::PanelPointer>   Panels;
			std::atomic<PanelRegistry::MenuTreePointer> Roots;
			Model::PanelHandle                          NextHandle{ 1 };
		};
		[[nodiscard]] PanelRegistryState* GetPanelRegistryState() noexcept
		{
			static auto* registry = new (std::nothrow) PanelRegistryState();
			return registry;
		}
		[[nodiscard]] bool IsValidText(
			const Model::StringView& a_text,
			std::uint32_t            a_maximumLength) noexcept
		{
			return a_text.Data && a_text.Size && a_text.Size <= a_maximumLength &&
				!std::memchr(a_text.Data, '\0', a_text.Size);
		}
		void AddToMenuTree(
			PanelRegistryState&                          a_registry,
			const PanelRegistry::PanelPointer& a_panel)
		{
			const std::string path = a_panel->Section + '/' + a_panel->Title;
			const std::string_view pathView{ path };
			std::vector<std::string_view> parts;
			for (std::size_t begin = 0; begin < path.size();) {
				const auto slash = path.find('/', begin);
				parts.push_back(pathView.substr(
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
				node->Panel.store(a_panel, std::memory_order_release);
				return;
			}
			// Allocate the complete missing branch before publishing its root.
			// A failed allocation therefore cannot expose partial empty nodes.
			PanelRegistry::MenuNodePointer branch;
			for (auto index = parts.size(); index-- > firstMissing;) {
				node = std::make_shared<PanelRegistry::MenuNode>();
				node->Name = parts[index];
				const auto prefixLength = static_cast<std::size_t>(
					parts[index].data() - path.data()) + parts[index].size();
				node->FullPath.assign(path.data(), prefixLength);
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
		}
	}

	Model::RegistrationResult PanelRegistry::Register(
		const Model::PanelRegistration* a_registration,
		Model::PanelHandle*              a_handle) noexcept
	{
		if (a_handle) {
			*a_handle = 0;
		}
		if (!a_registration) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (a_registration->StructureSize < sizeof(Model::PanelRegistration) ||
			a_registration->ImGui.StructureSize < sizeof(Model::ImGuiLayout)) {
			return Model::RegistrationResult::StructureTooSmall;
		}
		if (a_registration->InterfaceVersion != Model::INTERFACE_VERSION) {
			return Model::RegistrationResult::UnsupportedVersion;
		}
		if (!a_registration->Render ||
			!IsValidText(a_registration->Id, Model::MAXIMUM_PANEL_ID_LENGTH) ||
			!IsValidText(a_registration->Section, Model::MAXIMUM_PANEL_TEXT_LENGTH) ||
			!IsValidText(a_registration->Title, Model::MAXIMUM_PANEL_TEXT_LENGTH)) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (!Detail::HasMatchingImGuiLayout(a_registration->ImGui)) {
			return Model::RegistrationResult::ImGuiMismatch;
		}
		void* ownerModule{};
		if (!Detail::IsExecutableImageFunction(a_registration->Render, &ownerModule)) {
			return Model::RegistrationResult::InvalidArgument;
		}
		try {
			auto panel = std::make_shared<Panel>();
			panel->OwnerModule = ownerModule;
			panel->Id.assign(a_registration->Id.Data, a_registration->Id.Size);
			panel->Section.assign(
				a_registration->Section.Data, a_registration->Section.Size);
			panel->Title.assign(
				a_registration->Title.Data, a_registration->Title.Size);
			panel->Render = a_registration->Render;
			panel->UserData = a_registration->UserData;
			auto* registry = GetPanelRegistryState();
			if (!registry) {
				return Model::RegistrationResult::OutOfMemory;
			}
			std::scoped_lock lock{ registry->Mutex };
			if (registry->Panels.size() >= maximumPanelCount) {
				return Model::RegistrationResult::RegistryFull;
			}
			registry->Panels.reserve(maximumPanelCount);
			if (std::ranges::any_of(registry->Panels, [&](const auto& registered) {
					return registered->OwnerModule == panel->OwnerModule &&
						registered->Id == panel->Id;
				})) {
				return Model::RegistrationResult::DuplicateId;
			}
			const auto registeredHandle = registry->NextHandle++;
			AddToMenuTree(*registry, panel);
			registry->Panels.push_back(std::move(panel));
			if (a_handle) {
				*a_handle = registeredHandle;
			}
			return Model::RegistrationResult::Success;
		} catch (const std::bad_alloc&) {
			return Model::RegistrationResult::OutOfMemory;
		} catch (const std::system_error&) {
			return Model::RegistrationResult::InternalError;
		}
	}

	PanelRegistry::MenuTreePointer PanelRegistry::GetMenuTree() noexcept
	{
		const auto* registry = GetPanelRegistryState();
		return registry ? registry->Roots.load(std::memory_order_acquire) : nullptr;
	}

	void PanelRegistry::Render(
		const PanelPointer&         a_panel,
		const Model::RenderContext& a_context)
	{
		if (!a_panel ||
			!a_panel->Enabled.load(std::memory_order_acquire) ||
			!a_panel->Render) {
			return;
		}
		const auto result = a_panel->Render(&a_context, a_panel->UserData);
		if (result == Model::PanelRenderResult::Continue) {
			return;
		}
		a_panel->Enabled.store(false, std::memory_order_release);
		if (result == Model::PanelRenderResult::Failed) {
			logger::error("Disabled panel '{} / {}' after its render callback failed",
				a_panel->Section, a_panel->Title);
		} else {
			logger::info("Panel '{} / {}' disabled itself",
				a_panel->Section, a_panel->Title);
		}
	}
}
