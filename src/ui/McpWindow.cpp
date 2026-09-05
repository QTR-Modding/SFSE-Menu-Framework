#include "ui/McpWindow.h"

#include "appearance/FontManager.h"
#include "config/FrameworkSettings.h"
#include "config/RootMenuConfig.h"
#include "platform/win32/Win32Platform.h"
#include "runtime/PanelRegistry.h"
#include "runtime/WindowManager.h"
#include "ui/SettingsWindow.h"
#include "ui/WindowPresentation.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	constexpr char MCP_WINDOW_ID[] = "#MCPMainWindow";
	constexpr char MCP_TITLE[] = "Mod Control Panel";
	constexpr char menuConfigPath[] =
		"Data\\SFSE\\Plugins\\SFSEMenuFrameworkMenuConfig.json";

	using PanelPointer = SFSEMenuFramework::PanelRegistry::PanelPointer;
	using MenuNode = SFSEMenuFramework::PanelRegistry::MenuNode;
	using MenuNodePointer = SFSEMenuFramework::PanelRegistry::MenuNodePointer;
	using MenuTreePointer = SFSEMenuFramework::PanelRegistry::MenuTreePointer;

	// The tree/filter/favorites/archive shell below directly adapts
	// SKSE Menu Framework 3 UI.cpp and RootMenuConfig.cpp at commit
	// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
	// Process-lifetime nodes and copy-on-write child lists preserve its direct
	// registration tree while remaining safe for cross-thread registrations.
	ImGuiTextFilter rootFilter;
	MenuNodePointer selectedNode;
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
		return a_panel && a_panel->Render;
	}

	[[nodiscard]] PanelPointer GetPanel(const MenuNode& a_node)
	{
		return a_node.Panel.load(std::memory_order_acquire);
	}

	[[nodiscard]] bool HasEnabledChild(const MenuNode& a_node);

	[[nodiscard]] bool HasEnabledPanel(const MenuNode& a_node)
	{
		return IsPanelEnabled(GetPanel(a_node)) || HasEnabledChild(a_node);
	}

	[[nodiscard]] bool HasEnabledChild(const MenuNode& a_node)
	{
		const auto children = a_node.Children.load(std::memory_order_acquire);
		return children && std::ranges::any_of(
			*children,
			[](const auto& a_child) {
				return a_child && HasEnabledPanel(*a_child);
			});
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

	void SetRootMenuArchived(
		std::string_view a_menuName,
		bool             a_archived)
	{
		const bool saved = SFSEMenuFramework::RootMenuConfig::SetArchived(
			a_menuName,
			a_archived);
		menuConfigSaveFailed = !saved;
		if (!saved) {
			return;
		}
		if (a_archived && selectedNode &&
			selectedNode->FullPath.starts_with(a_menuName) &&
			selectedNode->FullPath.size() > a_menuName.size() &&
			selectedNode->FullPath[a_menuName.size()] == '/') {
			selectedNode.reset();
		}
	}

	void RenderFavoriteStar(bool a_favorite)
	{
		if (!ImGui::IsItemVisible()) {
			return;
		}

		// Clockwise star outline, independent of the selected font and glyph ranges.
		ImVec2 points[]{
			{ 0.50F, 0.00F }, { 0.62F, 0.35F }, { 1.00F, 0.35F },
			{ 0.69F, 0.58F }, { 0.81F, 0.95F }, { 0.50F, 0.72F },
			{ 0.19F, 0.95F }, { 0.31F, 0.58F }, { 0.00F, 0.35F },
			{ 0.38F, 0.35F }
		};
		const auto minimum = ImGui::GetItemRectMin();
		const auto maximum = ImGui::GetItemRectMax();
		const float buttonSize = maximum.y - minimum.y;
		const float iconSize = buttonSize * 0.60F;
		for (auto& point : points) {
			point.x = (minimum.x + maximum.x - iconSize) * 0.5F + point.x * iconSize;
			point.y = (minimum.y + maximum.y - iconSize * 0.95F) * 0.5F + point.y * iconSize;
		}

		const auto color = ImGui::GetColorU32(a_favorite ?
			ImVec4{ 1.0F, 0.84F, 0.0F, 1.0F } :
			ImLerp(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.65F));
		auto* drawList = ImGui::GetWindowDrawList();
		if (a_favorite) {
			drawList->AddConcavePolyFilled(points, IM_ARRAYSIZE(points), color);
		} else {
			drawList->AddPolyline(points, IM_ARRAYSIZE(points), color,
				ImDrawFlags_Closed, (std::max)(1.5F, buttonSize * 0.045F));
		}
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
		if (ImGui::Button("##Favorite", ImVec2{ buttonSize, buttonSize })) {
			menuConfigSaveFailed =
				!SFSEMenuFramework::RootMenuConfig::SetFavorite(
					a_menuName,
					!a_favorite);
		}
		RenderFavoriteStar(a_favorite);
		RenderTooltip(
			a_favorite ? "Remove from favorites" : "Add to favorites");

		ImGui::SetCursorScreenPos(
			ImVec2{
				headerMaximum.x - buttonSize,
				headerMinimum.y
			});
		if (ImGui::Button("-", ImVec2{ buttonSize, buttonSize })) {
			pendingArchiveMenu = a_menuName;
			archiveConfirmationRequested = true;
		}
		RenderTooltip("Archive menu");

		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		ImGui::PopID();
		ImGui::SetCursorScreenPos(restoreCursor);
	}

	void RenderLiteralTextClipped(
		std::string_view a_text,
		const ImVec2&    a_minimum,
		const ImVec2&    a_maximum)
	{
		if (a_text.empty() || a_maximum.x <= a_minimum.x ||
			a_maximum.y <= a_minimum.y) {
			return;
		}

		const auto* begin = a_text.data();
		const auto* end = begin + a_text.size();
		const auto size = ImGui::CalcTextSize(begin, end, false);
		ImGui::RenderTextClippedEx(
			ImGui::GetWindowDrawList(),
			a_minimum,
			a_maximum,
			begin,
			end,
			&size);
	}

	void RenderNode(const MenuNodePointer& a_node)
	{
		if (!a_node || !HasEnabledPanel(*a_node)) {
			return;
		}

		const auto panel = GetPanel(*a_node);
		const bool hasEnabledChild = HasEnabledChild(*a_node);
		auto flags = baseTreeNodeFlags;
		if (selectedNode == a_node && IsPanelEnabled(panel)) {
			flags |= ImGuiTreeNodeFlags_Selected;
		}
		if (!hasEnabledChild) {
			flags |=
				ImGuiTreeNodeFlags_Leaf |
				ImGuiTreeNodeFlags_NoTreePushOnOpen;
		}

		const auto* labelBegin = a_node->Name.data();
		const auto* labelEnd = labelBegin + a_node->Name.size();
		const bool nodeOpen = ImGui::TreeNodeBehavior(
			ImGui::GetCurrentWindow()->GetID(a_node.get()),
			flags,
			labelBegin,
			labelEnd);
		const bool itemClicked = ImGui::IsItemClicked();
		const bool itemToggledOpen = ImGui::IsItemToggledOpen();
		const bool gamePadButtonPressed =
			ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown);
		const bool itemFocused = ImGui::IsItemFocused();

		if ((itemClicked || (gamePadButtonPressed && itemFocused)) &&
			!itemToggledOpen &&
			IsPanelEnabled(panel)) {
			selectedNode = a_node;
		}

		if (nodeOpen && hasEnabledChild) {
			const auto children =
				a_node->Children.load(std::memory_order_acquire);
			for (const auto& child : *children) {
				RenderNode(child);
			}
			ImGui::TreePop();
		}
	}

	void RenderArchivedMenuRecovery()
	{
		if (!ImGui::BeginMenu("Restore Archived Menus")) {
			return;
		}

		const auto roots =
			SFSEMenuFramework::PanelRegistry::GetMenuTree();
		std::vector<MenuNodePointer> archivedMenus;
		if (roots) {
			archivedMenus.reserve(roots->size());
			for (const auto& root : *roots) {
				if (SFSEMenuFramework::RootMenuConfig::IsArchived(
						root->Name)) {
					archivedMenus.push_back(root);
				}
			}
		}
		std::ranges::sort(
			archivedMenus,
			{},
			[](const auto& a_root) -> const std::string& {
				return a_root->Name;
			});

		for (const auto& root : archivedMenus) {
			const auto* begin = root->Name.data();
			const auto* end = begin + root->Name.size();
			const auto textSize = ImGui::CalcTextSize(begin, end, false);
			auto* window = ImGui::GetCurrentWindow();
			const ImVec2 textMinimum{
				window->DC.CursorPos.x,
				window->DC.CursorPos.y + window->DC.CurrLineTextBaseOffset
			};

			ImGui::PushID(root.get());
			const bool restore = ImGui::Selectable(
				"##RestoreArchivedMenu",
				false,
				ImGuiSelectableFlags_SpanAvailWidth,
				textSize);
			RenderLiteralTextClipped(
				root->Name,
				textMinimum,
				ImGui::GetItemRectMax());
			ImGui::PopID();

			if (restore) {
				SetRootMenuArchived(root->Name, false);
			}
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
			SetRootMenuArchived(pendingArchiveMenu, true);
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
				resetMainWindowPlacement = true;
				SFSEMenuFramework::SettingsWindow::ResetPlacement();
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

		const float textWidth = ImGui::CalcTextSize(MCP_TITLE).x;
		const float availableWidth =
			ImGui::GetWindowWidth() - ImGui::GetFrameHeight() -
			ImGui::GetStyle().ItemSpacing.x;
		const float titlePosition =
			availableWidth * 0.5F - textWidth * 0.5F;

		ImGui::SameLine(titlePosition);
		ImGui::TextUnformatted(MCP_TITLE);
		if (SFSEMenuFramework::UI::RenderCloseButton()) {
			SFSEMenuFramework::SettingsWindow::Close();
			static_cast<void>(
				SFSEMenuFramework::WindowManager::SetMainWindowOpen(false));
		}
		ImGui::EndMenuBar();
	}

	void RenderNavigation()
	{
		const auto roots = SFSEMenuFramework::PanelRegistry::GetMenuTree();
		auto selectedPanel = selectedNode ? GetPanel(*selectedNode) : nullptr;
		if (!IsPanelEnabled(selectedPanel)) {
			selectedPanel.reset();
			selectedNode.reset();
		}

		const auto available = ImGui::GetContentRegionAvail();
		const float navigationWidth = available.x * 0.3F;
		const float uiScale = SFSEMenuFramework::FontManager::GetActiveInfo().Settings.UIScale;
		const float filterHeight = 50.0F * uiScale;
		const float headerHeight = 41.0F * uiScale;
		const float headerOffsetY = 5.0F * uiScale;

		if (ImGui::BeginChild(
				"TreeView2", ImVec2{ navigationWidth, filterHeight }, ImGuiChildFlags_None)) {
			rootFilter.Draw("##SFSEModControlPanelMenuFilter", -FLT_MIN);
		}
		ImGui::EndChild();

		ImGui::SameLine();
		if (ImGui::BeginChild(
				"SFSEModControlPanelModMenuHeader", ImVec2{ 0.0F, headerHeight },
				ImGuiChildFlags_None)) {
			if (selectedPanel) {
				const std::string_view title = selectedNode->Name;
				const float windowWidth = ImGui::GetWindowSize().x;
				const float textWidth =
					ImGui::CalcTextSize(title.data(), title.data() + title.size()).x;
				ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5F);
				ImGui::SetCursorPosY(headerOffsetY);
				ImGui::TextUnformatted(title.data(), title.data() + title.size());
			}
		}
		ImGui::EndChild();

		if (ImGui::BeginChild(
				"SFSEModControlPanelTreeView", ImVec2{ navigationWidth, -FLT_MIN },
				ImGuiChildFlags_Border)) {
			ImGui::PushStyleVar(
				ImGuiStyleVar_FramePadding,
				ImVec2{ 0.0F, 5.0F * uiScale });

			std::vector<MenuNodePointer> rootMenus;
			if (roots) {
				rootMenus.reserve(roots->size());
				for (const auto& root : *roots) {
					if (root && HasEnabledChild(*root)) {
						rootMenus.push_back(root);
					}
				}
			}
			std::stable_sort(rootMenus.begin(), rootMenus.end(),
				[](const auto& a_left, const auto& a_right) {
					const bool leftFavorite =
						SFSEMenuFramework::RootMenuConfig::IsFavorite(a_left->Name);
					const bool rightFavorite =
						SFSEMenuFramework::RootMenuConfig::IsFavorite(a_right->Name);
					if (leftFavorite != rightFavorite) {
						return leftFavorite;
					}
					return a_left->Name < a_right->Name;
				});

			for (const auto& root : rootMenus) {
				if (SFSEMenuFramework::RootMenuConfig::IsArchived(root->Name)) {
					continue;
				}

				const bool favorite = SFSEMenuFramework::RootMenuConfig::IsFavorite(root->Name);
				const bool passesFilter = rootFilter.PassFilter(root->Name.c_str());
				constexpr ImGuiTreeNodeFlags headerFlags =
					ImGuiTreeNodeFlags_AllowOverlap;
				bool headerOpen{};
				if (passesFilter) {
					auto* window = ImGui::GetCurrentWindow();
					const auto& style = ImGui::GetStyle();
					const ImVec2 textMinimum{
						window->DC.CursorPos.x + ImGui::GetFontSize() +
							style.FramePadding.x * 3.0F,
						window->DC.CursorPos.y + (std::max)(
							style.FramePadding.y,
							window->DC.CurrLineTextBaseOffset)
					};

					ImGui::PushID(root.get());
					headerOpen = ImGui::CollapsingHeader(
						"##RootMenuHeader", headerFlags);
					const auto headerMinimum = ImGui::GetItemRectMin();
					const auto headerMaximum = ImGui::GetItemRectMax();
					const float buttonSize = headerMaximum.y - headerMinimum.y;
					RenderLiteralTextClipped(
						root->Name,
						textMinimum,
						ImVec2{
							headerMaximum.x - buttonSize * 2.0F,
							headerMaximum.y
						});
					RenderRootMenuButtons(root->Name, favorite);
					ImGui::PopID();
				}
				if (headerOpen) {
					const auto children = root->Children.load(std::memory_order_acquire);
					for (const auto& child : *children) {
						RenderNode(child);
					}
				}
			}

			if (menuConfigSaveFailed) {
				ImGui::Spacing();
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F }, "Could not save %s", menuConfigPath);
			}
			ImGui::PopStyleVar();
		}
		ImGui::EndChild();

		ImGui::SameLine();
		if (ImGui::BeginChild(
				"SFSEModControlPanelMenuNode", ImVec2{ 0.0F, -FLT_MIN },
				ImGuiChildFlags_Border)) {
			if (selectedPanel) {
				SFSEMenuFramework::PanelRegistry::Render(selectedPanel);
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

void __stdcall SFSEMenuFramework::McpWindow::Render()
{
	ObserveMainOpenSession();
	const auto* viewport = ImGui::GetMainViewport();
	SFSEMenuFramework::UI::ApplyWindowPlacement(
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
		RenderNavigation();
		RenderArchiveConfirmation();
	}
	ImGui::End();

	SettingsWindow::Render();
}
