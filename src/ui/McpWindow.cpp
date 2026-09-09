#include "ui/McpWindow.h"

#include "appearance/FontManager.h"
#include "appearance/ThemeManager.h"
#include "config/FrameworkSettings.h"
#include "config/RootMenuConfig.h"
#include "input/GamepadNavigation.h"
#include "platform/win32/Win32Platform.h"
#include "runtime/PanelRegistry.h"
#include "runtime/WindowManager.h"
#include "ui/SettingsWindow.h"
#include "ui/WindowPlacement.h"
#include "ui/WindowPresentation.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	constexpr char MCP_TITLE[] = "Mod Control Panel";
	constexpr char menuConfigPath[] =
		"Data\\SFSE\\Plugins\\SFSEMenuFrameworkMenuConfig.json";

	using PanelPointer = SFSEMenuFramework::PanelRegistry::PanelPointer;
	using MenuNode = SFSEMenuFramework::PanelRegistry::MenuNode;
	using MenuNodePointer = SFSEMenuFramework::PanelRegistry::MenuNodePointer;
	using MenuTreePointer = SFSEMenuFramework::PanelRegistry::MenuTreePointer;
	using RootState = SFSEMenuFramework::PanelRegistry::RootState;

	// The tree/filter/favorites/archive shell below directly adapts
	// SKSE Menu Framework 3 UI.cpp and RootMenuConfig.cpp through commit
	// accepted base 8a366c4a3db7317655cec8379e48c43140c9fe7d (GPL-3.0).
	// Reference-counted snapshots and copy-on-write child lists preserve its
	// direct registration tree while allowing callback-safe mutations.
	ImGuiTextFilter rootFilter;
	MenuNodePointer selectedNode;
	MenuNodePointer pendingArchiveMenu;
	bool archiveConfirmationRequested{};
	bool menuConfigSaveFailed{};

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

	[[nodiscard]] MenuNodePointer FindNodeByIdentity(
		const MenuNodePointer& a_node,
		std::uint64_t          a_identity)
	{
		if (!a_node) {
			return nullptr;
		}
		if (a_node->Identity == a_identity) {
			return a_node;
		}
		const auto children =
			a_node->Children.load(std::memory_order_acquire);
		if (!children) {
			return nullptr;
		}
		for (const auto& child : *children) {
			if (auto found = FindNodeByIdentity(child, a_identity)) {
				return found;
			}
		}
		return nullptr;
	}

	[[nodiscard]] MenuNodePointer FindNodeByIdentity(
		const MenuTreePointer& a_nodes,
		std::uint64_t          a_identity)
	{
		if (!a_nodes || a_identity == 0) {
			return nullptr;
		}
		for (const auto& node : *a_nodes) {
			if (auto found = FindNodeByIdentity(node, a_identity)) {
				return found;
			}
		}
		return nullptr;
	}

	[[nodiscard]] bool ContainsNode(
		const MenuNodePointer& a_root,
		const MenuNodePointer& a_node)
	{
		return a_node &&
			FindNodeByIdentity(a_root, a_node->Identity) != nullptr;
	}

	void PushNodeID(std::uint64_t a_identity)
	{
		ImGui::PushID(static_cast<int>(a_identity >> 32));
		ImGui::PushID(static_cast<int>(a_identity));
	}

	void PopNodeID()
	{
		ImGui::PopID();
		ImGui::PopID();
	}

	[[nodiscard]] ImGuiID GetNodeID(std::uint64_t a_identity)
	{
		PushNodeID(a_identity);
		const auto id = ImGui::GetID("##MenuNode");
		PopNodeID();
		return id;
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
		const MenuNodePointer& a_menu,
		bool                   a_archived)
	{
		if (!a_menu) {
			return;
		}
		const bool saved = SFSEMenuFramework::PanelRegistry::SetRootArchived(
			a_menu->Identity,
			a_archived);
		menuConfigSaveFailed = !saved;
		if (!saved) {
			return;
		}
		if (a_archived && ContainsNode(a_menu, selectedNode)) {
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

	void RenderRootMenuActions(
		const MenuNodePointer& a_menu,
		bool                   a_favorite,
		float                  a_buttonSize)
	{
		PushNodeID(a_menu->Identity);
		ImGui::PushStyleColor(
			ImGuiCol_Button,
			ImVec4{ 0.0F, 0.0F, 0.0F, 0.0F });

		ImGui::TableSetColumnIndex(1);
		if (ImGui::Button(
				"##Favorite",
				ImVec2{ a_buttonSize, a_buttonSize })) {
			menuConfigSaveFailed =
				!SFSEMenuFramework::PanelRegistry::SetRootFavorite(
					a_menu->Identity,
					!a_favorite);
		}
		RenderFavoriteStar(a_favorite);
		RenderTooltip(
			a_favorite ? "Remove from favorites" : "Add to favorites");

		ImGui::TableSetColumnIndex(2);
		if (ImGui::Button("-", ImVec2{ a_buttonSize, a_buttonSize })) {
			pendingArchiveMenu = a_menu;
			archiveConfirmationRequested = true;
		}
		RenderTooltip("Archive menu");

		ImGui::PopStyleColor();
		PopNodeID();
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
			GetNodeID(a_node->Identity),
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
				const auto state =
					SFSEMenuFramework::PanelRegistry::GetRootState(
						root->Identity);
				if (state && state->Archived) {
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

			PushNodeID(root->Identity);
			const bool restore = ImGui::Selectable(
				"##RestoreArchivedMenu",
				false,
				ImGuiSelectableFlags_SpanAvailWidth,
				textSize);
			RenderLiteralTextClipped(
				root->Name,
				textMinimum,
				ImGui::GetItemRectMax());
			PopNodeID();

			if (restore) {
				SetRootMenuArchived(root, false);
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
		if (pendingArchiveMenu) {
			pendingArchiveMenu = FindNodeByIdentity(
				SFSEMenuFramework::PanelRegistry::GetMenuTree(),
				pendingArchiveMenu->Identity);
		}
		if (archiveConfirmationRequested && pendingArchiveMenu) {
			ImGui::OpenPopup(popupTitle);
		}
		archiveConfirmationRequested = false;

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
		if (!pendingArchiveMenu) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::TextUnformatted("Archive this menu?");
		ImGui::TextUnformatted(pendingArchiveMenu->Name.c_str());
		ImGui::Separator();
		if (ImGui::Button("Yes")) {
			SetRootMenuArchived(pendingArchiveMenu, true);
			pendingArchiveMenu.reset();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("No")) {
			pendingArchiveMenu.reset();
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

	void CloseMainWindow()
	{
		SFSEMenuFramework::SettingsWindow::Close();
		static_cast<void>(
			SFSEMenuFramework::WindowManager::SetMainWindowOpen(false));
	}

	void RenderMainMenuBar()
	{
		if (!ImGui::BeginMenuBar()) {
			return;
		}

		if (ImGui::BeginMenu("Options")) {
			if (ImGui::MenuItem("Reset Windows")) {
				SFSEMenuFramework::WindowPlacement::Reset();
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
			CloseMainWindow();
		}
		ImGui::EndMenuBar();
	}

	void RenderSearchFilter()
	{
		const float buttonSize = ImGui::GetFrameHeight();
		const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
		const float availableWidth = ImGui::GetContentRegionAvail().x;
		const bool showClear = availableWidth >= buttonSize * 2.0F + spacing;
		rootFilter.Draw("##SFSEModControlPanelMenuFilter",
			showClear ? availableWidth - buttonSize - spacing : -FLT_MIN);
		if (!showClear) {
			return;
		}

		ImGui::SameLine(0.0F, spacing);
		ImGui::BeginDisabled(rootFilter.InputBuf[0] == '\0');
		if (ImGui::Button("X##ClearSearch", ImVec2{ buttonSize, buttonSize })) {
			rootFilter.Clear();
		}
		ImGui::EndDisabled();
		RenderTooltip("Clear search");
	}

	void RenderNavigation()
	{
		const auto roots = SFSEMenuFramework::PanelRegistry::GetMenuTree();
		if (selectedNode) {
			selectedNode = FindNodeByIdentity(roots, selectedNode->Identity);
		}
		auto selectedPanel = selectedNode ? GetPanel(*selectedNode) : nullptr;
		if (!IsPanelEnabled(selectedPanel)) {
			selectedPanel.reset();
			selectedNode.reset();
		}

		const auto available = ImGui::GetContentRegionAvail();
		const float navigationWidth = available.x * 0.3F;
		const float uiScale = SFSEMenuFramework::FontManager::GetActiveInfo().Settings.UIScale;
		const float headerHeight = ImCeil((std::max)(50.0F * uiScale, ImGui::GetFrameHeight()));

		if (ImGui::BeginChild(
				"TreeView2", ImVec2{ navigationWidth, headerHeight }, ImGuiChildFlags_None)) {
			SFSEMenuFramework::ThemeManager::RenderCurrentWindowBackdrop();
			RenderSearchFilter();
		}
		ImGui::EndChild();

		ImGui::SameLine();
		ImGui::SetNextWindowContentSize(ImVec2{ 0.0F, headerHeight });
		if (ImGui::BeginChild(
				"SFSEModControlPanelModMenuHeader", ImVec2{ 0.0F, headerHeight },
				ImGuiChildFlags_None)) {
			SFSEMenuFramework::ThemeManager::RenderCurrentWindowBackdrop();
			if (selectedPanel) {
				const std::string_view title = selectedNode->Name;
				const auto headerSize = ImGui::GetWindowSize();
				const auto textSize =
					ImGui::CalcTextSize(title.data(), title.data() + title.size());
				ImGui::SetCursorPos(ImVec2{
					(headerSize.x - textSize.x) * 0.5F,
					(headerSize.y - textSize.y) * 0.5F });
				ImGui::TextUnformatted(title.data(), title.data() + title.size());
			}
		}
		ImGui::EndChild();

		if (ImGui::BeginChild(
				"SFSEModControlPanelTreeView", ImVec2{ navigationWidth, -FLT_MIN },
				ImGuiChildFlags_Border)) {
			SFSEMenuFramework::ThemeManager::RenderCurrentWindowBackdrop();
			ImGui::PushStyleVar(
				ImGuiStyleVar_FramePadding,
				ImVec2{ 0.0F, 5.0F * uiScale });

			struct RootMenuEntry final
			{
				MenuNodePointer Node;
				RootState       State;
			};
			std::vector<RootMenuEntry> rootMenus;
			if (roots) {
				rootMenus.reserve(roots->size());
				for (const auto& root : *roots) {
					const auto state = root ?
						SFSEMenuFramework::PanelRegistry::GetRootState(
							root->Identity) :
						std::nullopt;
					if (state && HasEnabledChild(*root)) {
						rootMenus.push_back({ root, *state });
					}
				}
			}
			std::stable_sort(rootMenus.begin(), rootMenus.end(),
				[](const auto& a_left, const auto& a_right) {
					if (a_left.State.Favorite != a_right.State.Favorite) {
						return a_left.State.Favorite;
					}
					return a_left.Node->Name < a_right.Node->Name;
				});

			for (const auto& entry : rootMenus) {
				const auto& root = entry.Node;
				if (entry.State.Archived) {
					continue;
				}

				const bool favorite = entry.State.Favorite;
				const bool passesFilter = rootFilter.PassFilter(root->Name.c_str());
				bool headerOpen{};
				if (passesFilter) {
					PushNodeID(root->Identity);
					constexpr ImGuiTableFlags rowFlags =
						ImGuiTableFlags_SizingStretchProp |
						ImGuiTableFlags_NoSavedSettings |
						ImGuiTableFlags_NoPadOuterX;
					if (ImGui::BeginTable("##RootMenuRow", 3, rowFlags)) {
						const float buttonSize = ImGui::GetFrameHeight();
						ImGui::TableSetupColumn(
							"##Header", ImGuiTableColumnFlags_WidthStretch);
						ImGui::TableSetupColumn(
							"##Favorite", ImGuiTableColumnFlags_WidthFixed,
							buttonSize);
						ImGui::TableSetupColumn(
							"##Archive", ImGuiTableColumnFlags_WidthFixed,
							buttonSize);
						ImGui::TableNextRow(ImGuiTableRowFlags_None, buttonSize);
						ImGui::TableSetColumnIndex(0);

						auto* window = ImGui::GetCurrentWindow();
						const auto& style = ImGui::GetStyle();
						const ImVec2 textMinimum{
							window->DC.CursorPos.x + ImGui::GetFontSize() +
								style.FramePadding.x * 3.0F,
							window->DC.CursorPos.y + (std::max)(
								style.FramePadding.y,
								window->DC.CurrLineTextBaseOffset)
						};
						headerOpen = ImGui::CollapsingHeader("##RootMenuHeader");
						RenderLiteralTextClipped(
							root->Name,
							textMinimum,
							ImGui::GetItemRectMax());
						RenderRootMenuActions(root, favorite, buttonSize);
						ImGui::EndTable();
					}
					PopNodeID();
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
			SFSEMenuFramework::ThemeManager::RenderCurrentWindowBackdrop();
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
	WindowPlacement::Apply(WindowPlacement::BuiltInWindow::Main);

	constexpr ImGuiWindowFlags windowFlags =
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_MenuBar |
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoSavedSettings;
	const bool drawContents =
		ImGui::Begin(WindowPlacement::GetName(WindowPlacement::BuiltInWindow::Main),
			nullptr, windowFlags);
	WindowPlacement::Capture(WindowPlacement::BuiltInWindow::Main);
	if (drawContents) {
		ThemeManager::RenderCurrentWindowBackdrop();
	}
	const bool closeRequested =
		GamepadNavigation::ConsumeCloseRequestForCurrentWindow(
			WindowManager::GetBlockingWindowOpenGeneration());
	if (closeRequested) {
		CloseMainWindow();
	} else if (drawContents) {
		RenderMainMenuBar();
		RenderNavigation();
		RenderArchiveConfirmation();
	}
	ImGui::End();

	SettingsWindow::Render();
}
