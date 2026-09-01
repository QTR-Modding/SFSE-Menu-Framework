#include "McpWindow.h"

#include "FontManager.h"
#include "FrameworkSettings.h"
#include "PanelRegistry.h"
#include "RootMenuConfig.h"
#include "SettingsWindow.h"
#include "Win32Platform.h"
#include "WindowManager.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	constexpr char MCP_WINDOW_ID[] = "#MCPMainWindow";
	constexpr char MCP_TITLE[] = "Mod Control Panel";
	constexpr char menuConfigPath[] =
		"Data\\SFSE\\Plugins\\SFSEMenuFrameworkMenuConfig.json";

	using PanelHandle = SFSEMenuFramework::Model::PanelHandle;
	using PanelPointer = SFSEMenuFramework::PanelRegistry::PanelPointer;
	using PanelSnapshotPointer = SFSEMenuFramework::PanelRegistry::SnapshotPointer;

	struct MenuTreeNode final
	{
		std::string                                Name;
		std::string                                FullPath;
		PanelPointer                               Panel;
		std::vector<std::unique_ptr<MenuTreeNode>> Children;
	};

	struct RootMenuNode final
	{
		std::string                                Name;
		std::vector<std::unique_ptr<MenuTreeNode>> Children;
	};

	struct MenuTreeCache final
	{
		PanelSnapshotPointer                       Source;
		std::vector<std::unique_ptr<RootMenuNode>> Roots;
	};

	// The tree/filter/favorites/archive shell below directly adapts
	// SKSE Menu Framework 3 UI.cpp and RootMenuConfig.cpp at commit
	// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
	// Stable handles and immutable panel snapshots replace its mutable raw-pointer
	// registration tree; English text is embedded for this Starfield port.
	MenuTreeCache menuTree;
	ImGuiTextFilter rootFilter;
	PanelHandle selectedPanelHandle{};
	std::string selectedRootMenu;
	std::string selectedPanelPath;
	std::string selectedPanelTitle;
	std::string pendingArchiveMenu;
	bool archiveConfirmationRequested{};
	bool resetMainWindowPlacement{};
	bool menuConfigSaveFailed{};
	std::uint64_t observedMainSessionGeneration{};

	constexpr ImGuiTreeNodeFlags baseTreeNodeFlags =
		ImGuiTreeNodeFlags_OpenOnArrow |
		ImGuiTreeNodeFlags_OpenOnDoubleClick |
		ImGuiTreeNodeFlags_SpanAvailWidth;

	[[nodiscard]] bool IsPanelEnabled(const PanelPointer& a_panel)
	{
		return a_panel &&
		       a_panel->Enabled.load(std::memory_order_acquire);
	}

	[[nodiscard]] std::vector<std::string> SplitPanelPath(
		const SFSEMenuFramework::PanelRegistry::Panel& a_panel)
	{
		std::string path;
		path.reserve(a_panel.Section.size() + a_panel.Title.size() + 1);
		path.append(a_panel.Section);
		path.push_back('/');
		path.append(a_panel.Title);

		// This is the same slash-path contract used by the pinned SDK:
		// SetSection(root) + AddSectionItem(path) becomes root/path.
		std::vector<std::string> parts;
		std::stringstream stream{ path };
		std::string part;
		while (std::getline(stream, part, '/')) {
			parts.push_back(std::move(part));
		}
		return parts;
	}

	[[nodiscard]] RootMenuNode* FindOrAddRoot(std::string_view a_name)
	{
		const auto existing = std::ranges::find_if(
			menuTree.Roots,
			[&](const auto& a_root) {
				return a_root && a_root->Name == a_name;
			});
		if (existing != menuTree.Roots.end()) {
			return existing->get();
		}

		auto root = std::make_unique<RootMenuNode>();
		root->Name = a_name;
		auto* result = root.get();
		menuTree.Roots.push_back(std::move(root));
		return result;
	}

	[[nodiscard]] MenuTreeNode* FindOrAddChild(
		std::vector<std::unique_ptr<MenuTreeNode>>& a_children,
		std::string_view                            a_name,
		std::string_view                            a_fullPath)
	{
		const auto existing = std::ranges::find_if(
			a_children,
			[&](const auto& a_child) {
				return a_child && a_child->Name == a_name;
			});
		if (existing != a_children.end()) {
			return existing->get();
		}

		auto child = std::make_unique<MenuTreeNode>();
		child->Name = a_name;
		child->FullPath = a_fullPath;
		auto* result = child.get();
		a_children.push_back(std::move(child));
		return result;
	}

	void RebuildMenuTree(const PanelSnapshotPointer& a_snapshot)
	{
		if (menuTree.Source == a_snapshot) {
			return;
		}

		menuTree.Source = a_snapshot;
		menuTree.Roots.clear();
		if (!a_snapshot) {
			return;
		}

		std::vector<PanelPointer> panels;
		panels.reserve(a_snapshot->size());
		for (const auto& panel : *a_snapshot) {
			if (panel) {
				panels.push_back(panel);
			}
		}
		std::ranges::stable_sort(
			panels,
			{},
			[](const auto& a_panel) {
				return a_panel->Handle;
			});

		for (const auto& panel : panels) {
			if (!panel) {
				continue;
			}

			auto path = SplitPanelPath(*panel);
			if (path.size() < 2) {
				continue;
			}

			auto* root = FindOrAddRoot(path.front());
			auto* children = &root->Children;
			MenuTreeNode* node{};
			std::string fullPath = path.front();
			for (std::size_t index = 1; index < path.size(); ++index) {
				fullPath.push_back('/');
				fullPath.append(path[index]);
				node = FindOrAddChild(*children, path[index], fullPath);
				children = &node->Children;
			}
			if (node) {
				// Matching the pinned tree, a later registration for the exact
				// same path supplies the leaf callback.
				node->Panel = panel;
			}
		}
	}

	[[nodiscard]] bool HasEnabledPanel(const MenuTreeNode& a_node)
	{
		if (IsPanelEnabled(a_node.Panel)) {
			return true;
		}
		return std::ranges::any_of(
			a_node.Children,
			[](const auto& a_child) {
				return a_child && HasEnabledPanel(*a_child);
			});
	}

	[[nodiscard]] bool HasEnabledPanel(const RootMenuNode& a_root)
	{
		return std::ranges::any_of(
			a_root.Children,
			[](const auto& a_child) {
				return a_child && HasEnabledPanel(*a_child);
			});
	}

	[[nodiscard]] bool HasEnabledChild(const MenuTreeNode& a_node)
	{
		return std::ranges::any_of(
			a_node.Children,
			[](const auto& a_child) {
				return a_child && HasEnabledPanel(*a_child);
			});
	}

	[[nodiscard]] PanelPointer FindPanelByHandle(
		const MenuTreeNode& a_node,
		PanelHandle         a_handle)
	{
		if (IsPanelEnabled(a_node.Panel) &&
			a_node.Panel->Handle == a_handle) {
			return a_node.Panel;
		}

		for (const auto& child : a_node.Children) {
			if (!child) {
				continue;
			}
			if (auto panel = FindPanelByHandle(*child, a_handle)) {
				return panel;
			}
		}
		return {};
	}

	[[nodiscard]] PanelPointer FindSelectedPanel()
	{
		if (selectedPanelHandle == 0) {
			return {};
		}

		for (const auto& root : menuTree.Roots) {
			if (!root) {
				continue;
			}
			for (const auto& child : root->Children) {
				if (!child) {
					continue;
				}
				if (auto panel =
						FindPanelByHandle(*child, selectedPanelHandle)) {
					return panel;
				}
			}
		}
		return {};
	}

	[[nodiscard]] PanelPointer FindPanelByPath(
		const MenuTreeNode& a_node,
		std::string_view    a_path)
	{
		if (a_node.FullPath == a_path && IsPanelEnabled(a_node.Panel)) {
			return a_node.Panel;
		}

		for (const auto& child : a_node.Children) {
			if (!child) {
				continue;
			}
			if (auto panel = FindPanelByPath(*child, a_path)) {
				return panel;
			}
		}
		return {};
	}

	[[nodiscard]] PanelPointer FindPanelByPath(std::string_view a_path)
	{
		if (a_path.empty()) {
			return {};
		}

		for (const auto& root : menuTree.Roots) {
			if (!root) {
				continue;
			}
			for (const auto& child : root->Children) {
				if (!child) {
					continue;
				}
				if (auto panel = FindPanelByPath(*child, a_path)) {
					return panel;
				}
			}
		}
		return {};
	}

	void ClearSelection()
	{
		selectedPanelHandle = 0;
		selectedRootMenu.clear();
		selectedPanelPath.clear();
		selectedPanelTitle.clear();
	}

	void RenderTooltip(const char* a_text)
	{
		if (!ImGui::IsItemHovered()) {
			return;
		}
		ImGui::BeginTooltip();
		ImGui::TextUnformatted(a_text);
		ImGui::EndTooltip();
	}

	[[nodiscard]] bool SetRootMenuArchived(
		std::string_view a_menuName,
		bool             a_archived)
	{
		const bool saved =
			SFSEMenuFramework::RootMenuConfig::SetArchived(
				a_menuName,
				a_archived);
		menuConfigSaveFailed = !saved;
		if (a_archived && selectedRootMenu == a_menuName) {
			ClearSelection();
		}
		return saved;
	}

	void RequestArchive(std::string_view a_menuName)
	{
		pendingArchiveMenu = a_menuName;
		archiveConfirmationRequested = true;
	}

	void RenderRootMenuButtons(
		std::string_view a_menuName,
		bool             a_favorite)
	{
		const auto headerMinimum = ImGui::GetItemRectMin();
		const auto headerMaximum = ImGui::GetItemRectMax();
		const auto restoreCursor = ImGui::GetCursorScreenPos();
		const float buttonSize = headerMaximum.y - headerMinimum.y;

		ImGui::PushID(
			a_menuName.data(),
			a_menuName.data() + a_menuName.size());
		ImGui::PushStyleVar(
			ImGuiStyleVar_FramePadding,
			ImVec2{ 0.0F, 0.0F });
		ImGui::PushStyleColor(
			ImGuiCol_Button,
			ImVec4{ 0.0F, 0.0F, 0.0F, 0.0F });

		ImGui::SetCursorScreenPos(
			ImVec2{
				headerMaximum.x - buttonSize * 2.0F,
				headerMinimum.y
			});
		ImGui::PushStyleColor(
			ImGuiCol_Text,
			a_favorite ?
				ImVec4{ 1.0F, 0.84F, 0.0F, 1.0F } :
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		if (ImGui::Button("*", ImVec2{ buttonSize, buttonSize })) {
			menuConfigSaveFailed =
				!SFSEMenuFramework::RootMenuConfig::SetFavorite(
					a_menuName,
					!a_favorite);
		}
		ImGui::PopStyleColor();
		RenderTooltip(
			a_favorite ? "Remove from favorites" : "Add to favorites");

		ImGui::SetCursorScreenPos(
			ImVec2{
				headerMaximum.x - buttonSize,
				headerMinimum.y
			});
		if (ImGui::Button("-", ImVec2{ buttonSize, buttonSize })) {
			RequestArchive(a_menuName);
		}
		RenderTooltip("Archive menu");

		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		ImGui::PopID();
		ImGui::SetCursorScreenPos(restoreCursor);
	}


	void RenderNode(
		MenuTreeNode&   a_node,
		std::string_view a_rootMenu)
	{
		if (!HasEnabledPanel(a_node)) {
			return;
		}

		const bool hasEnabledChild = HasEnabledChild(a_node);
		auto flags = baseTreeNodeFlags;
		if (selectedPanelHandle != 0 &&
			IsPanelEnabled(a_node.Panel) &&
			selectedPanelHandle == a_node.Panel->Handle) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}
		if (!hasEnabledChild) {
			flags |=
				ImGuiTreeNodeFlags_Leaf |
				ImGuiTreeNodeFlags_NoTreePushOnOpen;
		}

		const bool nodeOpen = ImGui::TreeNodeEx(
			a_node.FullPath.c_str(),
			flags,
			"%s",
			a_node.Name.c_str());
		const bool itemClicked = ImGui::IsItemClicked();
		const bool itemToggledOpen = ImGui::IsItemToggledOpen();
		const bool gamePadButtonPressed =
			ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown);
		const bool itemFocused = ImGui::IsItemFocused();

		if ((itemClicked || (gamePadButtonPressed && itemFocused)) &&
			!itemToggledOpen &&
			IsPanelEnabled(a_node.Panel)) {
			selectedPanelHandle = a_node.Panel->Handle;
			selectedRootMenu = a_rootMenu;
			selectedPanelPath = a_node.FullPath;
			selectedPanelTitle = a_node.Name;
		}

		if (nodeOpen && hasEnabledChild) {
			for (const auto& child : a_node.Children) {
				if (child) {
					RenderNode(*child, a_rootMenu);
				}
			}
			ImGui::TreePop();
		}
	}

	void RenderArchivedMenuRecovery()
	{
		if (!ImGui::BeginMenu("Restore Archived Menus")) {
			return;
		}

		std::vector<RootMenuNode*> archivedMenus;
		archivedMenus.reserve(menuTree.Roots.size());
		for (const auto& root : menuTree.Roots) {
			if (root &&
				SFSEMenuFramework::RootMenuConfig::IsArchived(root->Name)) {
				archivedMenus.push_back(root.get());
			}
		}
		std::ranges::sort(
			archivedMenus,
			{},
			[](const auto* a_root) -> const std::string& {
				return a_root->Name;
			});

		for (const auto* root : archivedMenus) {
			ImGui::PushID(root->Name.c_str());
			if (ImGui::MenuItem(root->Name.c_str())) {
				menuConfigSaveFailed =
					!SFSEMenuFramework::RootMenuConfig::SetArchived(
						root->Name,
						false);
			}
			ImGui::PopID();
		}

		if (archivedMenus.empty()) {
			ImGui::MenuItem("No archived menus", nullptr, false, false);
		}
		ImGui::EndMenu();
	}

	void RenderArchiveConfirmation()
	{
		constexpr char popupTitle[] =
			"Archive menu##ArchiveRootMenuConfirmation";
		if (archiveConfirmationRequested) {
			ImGui::OpenPopup(popupTitle);
			archiveConfirmationRequested = false;
		}

		const auto* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(
			viewport->GetCenter(),
			ImGuiCond_Appearing,
			ImVec2{ 0.5F, 0.5F });
		if (!ImGui::BeginPopupModal(
				popupTitle,
				nullptr,
				ImGuiWindowFlags_AlwaysAutoResize)) {
			return;
		}

		ImGui::TextUnformatted("Archive this menu?");
		ImGui::TextUnformatted(pendingArchiveMenu.c_str());
		ImGui::Separator();
		if (ImGui::Button("Yes")) {
			static_cast<void>(
				SetRootMenuArchived(pendingArchiveMenu, true));
			pendingArchiveMenu.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("No")) {
			pendingArchiveMenu.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void ApplyWindowPlacement(
		const ImGuiViewport& a_viewport,
		float                a_widthRatio,
		float                a_heightRatio,
		bool&                a_reset)
	{
		ImGui::SetNextWindowPos(
			a_viewport.GetCenter(),
			a_reset ? ImGuiCond_Always : ImGuiCond_FirstUseEver,
			ImVec2{ 0.5F, 0.5F });
		ImGui::SetNextWindowSize(
			ImVec2{
				a_viewport.Size.x * a_widthRatio,
				a_viewport.Size.y * a_heightRatio
			},
			a_reset ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
		a_reset = false;
	}

	void ResetBuiltInWindowPlacement()
	{
		resetMainWindowPlacement = true;
		SFSEMenuFramework::SettingsWindow::ResetPlacement();
	}

	void CloseMainWindow()
	{
		SFSEMenuFramework::SettingsWindow::Close();
		static_cast<void>(
			SFSEMenuFramework::WindowManager::SetMainWindowOpen(false));
	}

	void ResumeGame()
	{
		if (auto* mainWindow =
				SFSEMenuFramework::WindowManager::GetMainWindow()) {
			// This matches SKSE-MF's visible-but-nonmodal Resume behavior. The
			// routed next open restores blocking before ownership is acquired.
			mainWindow->BlockUserInput.store(false, std::memory_order_release);
			static_cast<void>(
				SFSEMenuFramework::Win32Platform::PostHostWindowCallback());
		}
	}

	void ObserveMainOpenSession()
	{
		const auto generation =
			SFSEMenuFramework::WindowManager::GetMainWindowSessionGeneration();
		if (generation == 0 || generation == observedMainSessionGeneration) {
			return;
		}

		observedMainSessionGeneration = generation;
		SFSEMenuFramework::SettingsWindow::Close();
	}

	void RenderMainMenuBar()
	{
		if (!ImGui::BeginMenuBar()) {
			return;
		}

		if (ImGui::BeginMenu("Options")) {
			if (ImGui::MenuItem("Reset Windows")) {
				ResetBuiltInWindowPlacement();
			}
			if (ImGui::MenuItem("Resume Game")) {
				ResumeGame();
			}
			if (ImGui::MenuItem("Open Settings")) {
				SFSEMenuFramework::SettingsWindow::Open();
			}
			ImGui::Separator();
			RenderArchivedMenuRecovery();
			ImGui::EndMenu();
		}

		const float barWidth = ImGui::GetWindowWidth();
		const float barHeight = ImGui::GetFrameHeight();
		const float textWidth = ImGui::CalcTextSize(MCP_TITLE).x;
		const float closeButtonSize = barHeight;
		const float padding = ImGui::GetStyle().ItemSpacing.x;
		const float availableWidth = barWidth - closeButtonSize - padding;
		const float titlePosition =
			availableWidth * 0.5F - textWidth * 0.5F;

		ImGui::SameLine(titlePosition);
		ImGui::TextUnformatted(MCP_TITLE);

		const float closeButtonPosition =
			barWidth - closeButtonSize - padding;
		ImGui::SameLine(closeButtonPosition);
		ImGui::PushStyleVar(
			ImGuiStyleVar_FramePadding,
			ImVec2{ 0.0F, 0.0F });
		if (ImGui::Button(
				"X",
				ImVec2{ closeButtonSize, closeButtonSize })) {
			CloseMainWindow();
		}
		ImGui::PopStyleVar();
		ImGui::EndMenuBar();
	}

	void RenderNavigation(
		const PanelSnapshotPointer&                       a_snapshot,
		const SFSEMenuFramework::Model::RenderContext& a_context)
	{
		RebuildMenuTree(a_snapshot);
		auto selectedPanel = FindSelectedPanel();
		if (selectedPanelHandle != 0 && !selectedPanel) {
			selectedPanel = FindPanelByPath(selectedPanelPath);
			if (selectedPanel) {
				// SKSE-MF selects the tree node itself. Preserve that behavior when
				// a later registration replaces the callback at the same path.
				selectedPanelHandle = selectedPanel->Handle;
			} else {
				ClearSelection();
			}
		}

		const auto available = ImGui::GetContentRegionAvail();
		const float navigationWidth = available.x * 0.3F;
		const float uiScale =
			SFSEMenuFramework::FontManager::GetActiveUIScale();
		const float filterHeight = 50.0F * uiScale;
		const float headerHeight = 41.0F * uiScale;
		const float headerOffsetY = 5.0F * uiScale;

		if (ImGui::BeginChild(
				"TreeView2",
				ImVec2{ navigationWidth, filterHeight },
				ImGuiChildFlags_None)) {
			rootFilter.Draw("##SFSEModControlPanelMenuFilter", -FLT_MIN);
		}
		ImGui::EndChild();

		ImGui::SameLine();
		if (ImGui::BeginChild(
				"SFSEModControlPanelModMenuHeader",
				ImVec2{ 0.0F, headerHeight },
				ImGuiChildFlags_None)) {
			if (selectedPanel) {
				const float windowWidth = ImGui::GetWindowSize().x;
				const float textWidth =
					ImGui::CalcTextSize(selectedPanelTitle.c_str()).x;
				ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5F);
				ImGui::SetCursorPosY(headerOffsetY);
				ImGui::TextUnformatted(selectedPanelTitle.c_str());
			}
		}
		ImGui::EndChild();

		if (ImGui::BeginChild(
				"SFSEModControlPanelTreeView",
				ImVec2{ navigationWidth, -FLT_MIN },
				ImGuiChildFlags_Border)) {
			ImGui::PushStyleVar(
				ImGuiStyleVar_FramePadding,
				ImVec2{ 0.0F, 5.0F * uiScale });

			std::vector<RootMenuNode*> rootMenus;
			rootMenus.reserve(menuTree.Roots.size());
			for (const auto& root : menuTree.Roots) {
				if (root && HasEnabledPanel(*root)) {
					rootMenus.push_back(root.get());
				}
			}
			std::stable_sort(
				rootMenus.begin(),
				rootMenus.end(),
				[](const auto* a_left, const auto* a_right) {
					const bool leftFavorite =
						SFSEMenuFramework::RootMenuConfig::IsFavorite(
							a_left->Name);
					const bool rightFavorite =
						SFSEMenuFramework::RootMenuConfig::IsFavorite(
							a_right->Name);
					if (leftFavorite != rightFavorite) {
						return leftFavorite;
					}
					return a_left->Name < a_right->Name;
				});

			for (auto* root : rootMenus) {
				if (SFSEMenuFramework::RootMenuConfig::IsArchived(
						root->Name)) {
					continue;
				}

				const bool favorite =
					SFSEMenuFramework::RootMenuConfig::IsFavorite(
						root->Name);
				const bool passesFilter =
					rootFilter.PassFilter(root->Name.c_str());
				const std::string headerLabel =
					root->Name + "##RootMenu-" + root->Name;
				constexpr ImGuiTreeNodeFlags headerFlags =
					static_cast<ImGuiTreeNodeFlags>(
						ImGuiTreeNodeFlags_AllowOverlap) |
					static_cast<ImGuiTreeNodeFlags>(
						ImGuiTreeNodeFlags_ClipLabelForTrailingButton);
				const bool headerOpen =
					passesFilter &&
					ImGui::CollapsingHeader(
						headerLabel.c_str(),
						headerFlags);

				if (passesFilter) {
					RenderRootMenuButtons(root->Name, favorite);
				}
				if (headerOpen) {
					for (const auto& child : root->Children) {
						if (child) {
							RenderNode(*child, root->Name);
						}
					}
				}
			}

			if (menuConfigSaveFailed) {
				ImGui::Spacing();
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F },
					"Could not save %s",
					menuConfigPath);
			}
			ImGui::PopStyleVar();
		}
		ImGui::EndChild();

		ImGui::SameLine();
		if (ImGui::BeginChild(
				"SFSEModControlPanelMenuNode",
				ImVec2{ 0.0F, -FLT_MIN },
				ImGuiChildFlags_Border)) {
			if (selectedPanel) {
				SFSEMenuFramework::PanelRegistry::Render(
					selectedPanel,
					a_context);
			}
		}
		ImGui::EndChild();
	}

}

bool SFSEMenuFramework::McpWindow::Install()
{
	if (WindowManager::GetMainWindow()) {
		return false;
	}

	if (!RootMenuConfig::Load()) {
		logger::warn(
			"The root-menu configuration was invalid; valid entries were retained where possible");
	}

	auto* windowInterface = WindowManager::AddWindow(&McpWindow::Render);
	if (!windowInterface || !WindowManager::SetMainWindow(windowInterface)) {
		return false;
	}

	windowInterface->BlockUserInput.store(true, std::memory_order_relaxed);
	windowInterface->PauseGame.store(
		FrameworkSettings::GetFreezeTimeOnMenu(),
		std::memory_order_relaxed);
	windowInterface->BlurBackground.store(
		FrameworkSettings::GetBlurBackgroundOnMenu(),
		std::memory_order_relaxed);
	return true;
}

void __stdcall SFSEMenuFramework::McpWindow::Render(
	const Model::RenderContext& a_context)
{
	ObserveMainOpenSession();
	const auto* viewport = ImGui::GetMainViewport();
	ApplyWindowPlacement(
		*viewport,
		0.8F,
		0.8F,
		resetMainWindowPlacement);

	constexpr ImGuiWindowFlags windowFlags =
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_MenuBar |
		ImGuiWindowFlags_NoTitleBar;
	const bool drawContents =
		ImGui::Begin(MCP_WINDOW_ID, nullptr, windowFlags);
	if (drawContents) {
		RenderMainMenuBar();
		RenderNavigation(PanelRegistry::GetSnapshot(), a_context);
		RenderArchiveConfirmation();
	}
	ImGui::End();

	SettingsWindow::Render();
}
