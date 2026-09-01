
// ---- Mod Control Panel ------------------------------------------------------------

#include "UI.h"

#include "Appearance.h"
#include "Config.h"
#include "FrameworkRuntime.h"
#include "Win32Platform.h"

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
		return a_panel &&
		       a_panel->Enabled.load(std::memory_order_acquire);
	}

	[[nodiscard]] PanelPointer GetPanel(const MenuNode& a_node)
	{
		return a_node.Panel.load(std::memory_order_acquire);
	}

	[[nodiscard]] bool HasEnabledPanel(const MenuNode& a_node)
	{
		if (IsPanelEnabled(GetPanel(a_node))) {
			return true;
		}
		const auto children = a_node.Children.load(std::memory_order_acquire);
		return children && std::ranges::any_of(
			*children,
			[](const auto& a_child) {
				return a_child && HasEnabledPanel(*a_child);
			});
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

	[[nodiscard]] bool RenderCloseButton()
	{
		const float size = ImGui::GetFrameHeight();
		ImGui::SameLine(
			ImGui::GetWindowWidth() - size - ImGui::GetStyle().ItemSpacing.x);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{});
		const bool close = ImGui::Button("X", ImVec2{ size, size });
		ImGui::PopStyleVar();
		return close;
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
		if (a_archived && selectedNode &&
			selectedNode->FullPath.starts_with(a_menuName) &&
			selectedNode->FullPath.size() > a_menuName.size() &&
			selectedNode->FullPath[a_menuName.size()] == '/') {
			selectedNode.reset();
		}
		return saved;
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
			pendingArchiveMenu = a_menuName;
			archiveConfirmationRequested = true;
		}
		RenderTooltip("Archive menu");

		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
		ImGui::PopID();
		ImGui::SetCursorScreenPos(restoreCursor);
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

		const bool nodeOpen = ImGui::TreeNodeEx(
			a_node->FullPath.c_str(),
			flags,
			"%s",
			a_node->Name.c_str());
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
		if (RenderCloseButton()) {
			SFSEMenuFramework::SettingsWindow::Close();
			static_cast<void>(
				SFSEMenuFramework::WindowManager::SetMainWindowOpen(false));
		}
		ImGui::EndMenuBar();
	}

	void RenderNavigation(const SFSEMenuFramework::Model::RenderContext& a_context)
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
				const std::string headerLabel = root->Name + "##RootMenu-" + root->Name;
				constexpr ImGuiTreeNodeFlags headerFlags =
					static_cast<ImGuiTreeNodeFlags>(
						ImGuiTreeNodeFlags_AllowOverlap) |
					static_cast<ImGuiTreeNodeFlags>(
						ImGuiTreeNodeFlags_ClipLabelForTrailingButton);
				const bool headerOpen = passesFilter &&
					ImGui::CollapsingHeader(headerLabel.c_str(), headerFlags);

				if (passesFilter) {
					RenderRootMenuButtons(root->Name, favorite);
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
				SFSEMenuFramework::PanelRegistry::Render(selectedPanel, a_context);
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
		RenderNavigation(a_context);
		RenderArchiveConfirmation();
	}
	ImGui::End();

	SettingsWindow::Render();
}

// ---- Settings window ------------------------------------------------------------

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <utility>

namespace SFSEMenuFramework::SettingsWindow
{
	namespace
	{
		// The separate settings-window presentation and toggle control directly
		// adapt SKSE Menu Framework 3 UI.cpp at commit
		// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0).
		// Persistence and Starfield ownership updates remain SFSE-specific.
		constexpr char WINDOW_ID[] = "Settings##Window";

		bool isOpen{};
		bool focusRequested{};
		bool resetPlacement{};
		bool fontSettingsRefreshRequested{ true };
		bool fontSettingsInvalid{};

		void ApplyRuntimeSettings()
		{
			if (auto* mainWindow = WindowManager::GetMainWindow()) {
				mainWindow->PauseGame.store(
					FrameworkSettings::GetFreezeTimeOnMenu(), std::memory_order_release);
				mainWindow->BlurBackground.store(
					FrameworkSettings::GetBlurBackgroundOnMenu(), std::memory_order_release);
				static_cast<void>(Win32Platform::PostHostWindowCallback());
			}
		}

		[[nodiscard]] bool SaveOrRestore(
			const FrameworkSettings::SettingsSnapshot& a_previous,
			bool                                       a_rebuildFonts,
			bool&                                      a_themeLoadFailed)
		{
			if (FrameworkSettings::Save()) {
				return true;
			}
			FrameworkSettings::RestoreSnapshot(a_previous);
			if (a_rebuildFonts) {
				fontSettingsInvalid = !FontManager::RequestAtlasRebuild(a_previous.Fonts);
				fontSettingsRefreshRequested = true;
			}
			a_themeLoadFailed = !ThemeManager::QueueConfiguredTheme();
			ApplyRuntimeSettings();
			return false;
		}

		[[nodiscard]] bool RenderToggleMode(
			const char* a_label, const char* a_id,
			FrameworkSettings::ToggleMode& a_current)
		{
			int selected = std::to_underlying(a_current);
			constexpr std::array names{ "SINGLEPRESS", "HOLD", "DOUBLEPRESS", "OFF" };
			ImGui::TextUnformatted(a_label);
			if (!ImGui::Combo(a_id, &selected, names.data(), static_cast<int>(names.size()))) {
				return false;
			}
			a_current = static_cast<FrameworkSettings::ToggleMode>(selected);
			return true;
		}

		[[nodiscard]] bool RenderBinding(
			const char* a_label, const char* a_id,
			std::span<const FrameworkSettings::Binding> a_bindings,
			std::uint32_t& a_current)
		{
			const auto current = std::ranges::find(
				a_bindings, a_current, &FrameworkSettings::Binding::Code);
			bool changed{};
			ImGui::TextUnformatted(a_label);
			if (ImGui::BeginCombo(
					a_id, current == a_bindings.end() ? "UNKNOWN" : current->Name.data())) {
				for (const auto& binding : a_bindings) {
					const bool selected = binding.Code == a_current;
					ImGui::PushID(static_cast<int>(binding.Code));
					if (ImGui::Selectable(binding.Name.data(), selected)) {
						a_current = binding.Code;
						changed = true;
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			return changed;
		}

		[[nodiscard]] bool ToggleButton(const char* a_label, bool* a_value)
		{
			const auto position = ImGui::GetCursorScreenPos();
			auto* drawList = ImGui::GetWindowDrawList();
			const float height = ImGui::GetFrameHeight();
			const float width = height * 1.8F;
			const float radius = height * 0.5F;

			ImGui::PushID(a_label);
			constexpr ImVec4 transparent{};
			ImGui::PushStyleColor(ImGuiCol_Header, transparent);
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
			const bool clicked = ImGui::Selectable(
				"##toggle", false, ImGuiSelectableFlags_None, ImVec2{ width, height });
			ImGui::PopStyleColor(3);
			ImGui::PopID();
			if (clicked) {
				*a_value = !*a_value;
			}

			const float offset = *a_value ? 1.0F : 0.0F;
			const auto background =
				ImGui::GetColorU32(*a_value ? ImGuiCol_ButtonActive : ImGuiCol_Button);
			drawList->AddRectFilled(
				position, ImVec2{ position.x + width, position.y + height },
				background, height * 0.5F);
			drawList->AddCircleFilled(
				ImVec2{
					position.x + radius + offset * (width - radius * 2.0F),
					position.y + radius
				},
				radius - 1.5F, IM_COL32(255, 255, 255, 255));

			ImGui::SameLine();
			ImGui::TextUnformatted(a_label);
			return clicked;
		}

		[[nodiscard]] bool MatchesLiveSettings(
			const FrameworkSettings::FontSettings& a_settings) noexcept
		{
			return FrameworkSettings::FontSettingsEqual(
				a_settings, FontManager::GetActiveInfo().Settings, 0.0001F, true);
		}

		[[nodiscard]] bool QueueLiveFontSettings(
			const FrameworkSettings::FontSettings& a_settings) noexcept
		{
			return FrameworkSettings::ValidateFontSettings(a_settings) &&
				FontManager::RequestAtlasRebuild(a_settings);
		}

		void FinishFontEdit(
			const FrameworkSettings::FontSettings& a_settings,
			bool                                   a_changed)
		{
			if (a_changed) {
				fontSettingsInvalid =
					!FrameworkSettings::ValidateFontSettings(a_settings);
			}
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				fontSettingsInvalid = !QueueLiveFontSettings(a_settings);
			}
		}

		void RenderFontSettings(bool& a_saveFailed)
		{
			static FrameworkSettings::FontSettings pending{};
			if (fontSettingsRefreshRequested) {
				pending = FontManager::GetActiveInfo().Settings;
				fontSettingsRefreshRequested = false;
				fontSettingsInvalid = false;
			}

			ImGui::SeparatorText("Fonts");
			ImGui::TextUnformatted("Primary font");
			const auto pendingFontName = FrameworkSettings::GetFontFileNameView(pending.PrimaryFont);

			const auto fonts = FontManager::GetFonts();
			const auto foundFont = std::ranges::find_if(fonts, [pendingFontName](const auto& font) {
				return FrameworkSettings::EqualsIgnoreCaseAscii(pendingFontName, font.Name);
			});
			const FontManager::FontEntry* pendingFont =
				foundFont == fonts.end() ? nullptr : &*foundFont;
			std::string unavailablePreview = pendingFontName.empty() ?
				"INVALID" : "<missing: " + std::string{ pendingFontName } + ">";
			const char* fontPreview = pendingFont ? pending.PrimaryFont.data() : unavailablePreview.c_str();

			if (fonts.empty()) {
				ImGui::BeginDisabled();
				ImGui::Button("No fonts found##PrimaryFont");
				ImGui::EndDisabled();
			} else if (ImGui::BeginCombo("##PrimaryFont", fontPreview)) {
				for (std::size_t index = 0; index < fonts.size(); ++index) {
					const auto& name = fonts[index].Name;
					const bool selected =
						FrameworkSettings::EqualsIgnoreCaseAscii(pendingFontName, name);
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::Selectable(name.c_str(), selected)) {
						if (!FrameworkSettings::CopyFontFileName(name, pending.PrimaryFont, false)) {
							fontSettingsInvalid = true;
						} else {
							pendingFont = &fonts[index];
							const auto weightAxis = FontManager::GetWeightAxis(index);
							if (weightAxis) {
								pending.FontWeight = std::clamp(
									pending.FontWeight, weightAxis->Minimum, weightAxis->Maximum);
							}
							fontSettingsInvalid = !QueueLiveFontSettings(pending);
						}
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}

			if (pendingFont && pendingFont->WeightAxis) {
				const auto& axis = *pendingFont->WeightAxis;
				ImGui::Text("Font weight (%.0f - %.0f)", axis.Minimum, axis.Maximum);
				const bool weightEdited = ImGui::SliderFloat(
					"##FontWeight", &pending.FontWeight, axis.Minimum, axis.Maximum,
					"%.0f", ImGuiSliderFlags_AlwaysClamp);
				FinishFontEdit(pending, weightEdited);
				ImGui::TextDisabled(
					"Variable font; built-in default weight %.0f.", axis.Default);
			} else if (pendingFont) {
				ImGui::TextDisabled("Font weight: fixed by this font file.");
			} else {
				ImGui::TextDisabled("Font weight: unavailable until the font is found.");
			}

			ImGui::Text("Font size (%.0f - %.0f)", pending.MinFontSize, pending.MaxFontSize);
			const bool fontSizeEdited = ImGui::InputFloat(
				"##FontSizeMedium", &pending.FontSizeMedium, 1.0F, 4.0F, "%.1f");
			FinishFontEdit(pending, fontSizeEdited);

			ImGui::TextUnformatted("UI scale");
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("Scales text, ImGui controls, and framework layout.");
			}
			int uiScalePercent = static_cast<int>(std::lround(pending.UIScale * 100.0F));
			const bool uiScaleEdited = ImGui::SliderInt(
				"##UIScale", &uiScalePercent, 75, 200, "%d%%");
			if (uiScaleEdited) {
				pending.UIScale = static_cast<float>(uiScalePercent) / 100.0F;
			}
			FinishFontEdit(pending, uiScaleEdited);

			const auto active = FontManager::GetActiveInfo();
			const auto activeScalePercent =
				static_cast<int>(std::lround(active.Settings.UIScale * 100.0F));
			if (active.WeightAxis) {
				ImGui::TextDisabled(
					"Active: %.*s | weight %.0f | %.1f px | %d%% | %.1f raster px",
					static_cast<int>(active.Name.size()), active.Name.data(),
					active.Settings.FontWeight, active.Settings.FontSizeMedium,
					activeScalePercent, active.RasterSize);
			} else {
				ImGui::TextDisabled(
					"Active: %.*s | %.1f px | %d%% | %.1f raster px",
					static_cast<int>(active.Name.size()), active.Name.data(),
					active.Settings.FontSizeMedium, activeScalePercent, active.RasterSize);
			}
			if (!active.FallbackReason.empty()) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.75F, 0.25F, 1.0F }, "Fallback: %.*s",
					static_cast<int>(active.FallbackReason.size()), active.FallbackReason.data());
			}

			const bool pendingValid = FrameworkSettings::ValidateFontSettings(pending);
			const bool canSave = pendingValid && !fontSettingsInvalid &&
				!FontManager::HasPendingAtlasRebuild() &&
				MatchesLiveSettings(pending);
			ImGui::BeginDisabled(!canSave);
			if (ImGui::Button("Save")) {
				const auto previous = FrameworkSettings::GetFontSettings();
				if (!FrameworkSettings::SetFontSettings(pending)) {
					fontSettingsInvalid = true;
				} else if (!FrameworkSettings::Save()) {
					static_cast<void>(FrameworkSettings::SetFontSettings(previous));
					a_saveFailed = true;
				} else {
					pending = FrameworkSettings::GetFontSettings();
					fontSettingsInvalid = false;
					a_saveFailed = false;
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Reset font settings")) {
				pending = FrameworkSettings::GetDefaultFontSettings();
				fontSettingsInvalid = !QueueLiveFontSettings(pending);
			}

			if (fontSettingsInvalid || !pendingValid) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F },
					"Font weight must be 1 - 1000 and size must be within the displayed range; "
					"font size x UI scale must be at most 96 px.");
			}
			const auto configured = FrameworkSettings::GetFontSettings();
			const auto applyError = FontManager::GetLastApplyError();
			if (pendingValid && FontManager::HasPendingAtlasRebuild()) {
				ImGui::TextColored(
					ImVec4{ 0.35F, 0.8F, 1.0F, 1.0F }, "Applying live font changes...");
			} else if (!applyError.empty()) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F }, "%.*s",
					static_cast<int>(applyError.size()), applyError.data());
			} else if (pendingValid && !MatchesLiveSettings(pending)) {
				ImGui::TextDisabled("Finish editing to apply the live preview.");
			} else if (!FrameworkSettings::FontSettingsEqual(
				pending, configured, 0.0001F, true)) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.75F, 0.25F, 1.0F },
					"Live preview applied; changes are not saved.");
			} else {
				ImGui::TextDisabled("Font, variable weight, and UI-scale changes apply live.");
			}
		}

		void RenderFrameworkSettings()
		{
			static bool saveFailed{};
			static bool themeLoadFailed{};
			const auto settingsBeforeRender = FrameworkSettings::CaptureSnapshot();
			bool themeChanged{};

			const auto themes = ThemeManager::GetThemes();
			const auto selectedTheme = ThemeManager::GetSelectedThemeIndex();
			const char* themePreview = selectedTheme < themes.size() ?
				themes[selectedTheme].Name.c_str() :
				"BUILT-IN DARK";
			ImGui::TextUnformatted("Menu style");
			if (themes.empty()) {
				ImGui::BeginDisabled();
				ImGui::Button("No themes found##MenuStyle");
				ImGui::EndDisabled();
			} else if (ImGui::BeginCombo("##MenuStyle", themePreview)) {
				for (std::size_t index = 0; index < themes.size(); ++index) {
					const bool selected = index == selectedTheme;
					ImGui::PushID(static_cast<int>(index));
					if (ImGui::Selectable(themes[index].Name.c_str(), selected)) {
						if (ThemeManager::QueueTheme(index)) {
							themeChanged = true;
							themeLoadFailed = false;
						} else {
							themeLoadFailed = true;
						}
					}
					if (selected) {
						ImGui::SetItemDefaultFocus();
					}
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}

			RenderFontSettings(saveFailed);
			auto edited = FrameworkSettings::CaptureSnapshot();
			bool changed{};

			struct ToggleSetting final
			{
				const char* Label;
				bool FrameworkSettings::SettingsSnapshot::* Value;
			};
			constexpr std::array toggles{
				ToggleSetting{ "Freeze time while menu is open", &FrameworkSettings::SettingsSnapshot::FreezeTimeOnMenu },
				ToggleSetting{ "Blur background while menu is open", &FrameworkSettings::SettingsSnapshot::BlurBackgroundOnMenu }
			};
			for (const auto& toggle : toggles) {
				changed = ToggleButton(toggle.Label, &(edited.*toggle.Value)) || changed;
			}

			struct InputSetting final
			{
				const char* ModeLabel;
				const char* ModeID;
				FrameworkSettings::ToggleMode FrameworkSettings::SettingsSnapshot::* Mode;
				const char* KeyLabel;
				const char* KeyID;
				std::span<const FrameworkSettings::Binding> Bindings;
				std::uint32_t FrameworkSettings::SettingsSnapshot::* Key;
			};
			const std::array inputs{
				InputSetting{ "Toggle mode (keyboard)", "##KeyboardToggleMode",
					&FrameworkSettings::SettingsSnapshot::Mode, "Toggle key (keyboard)",
					"##KeyboardToggleKey", FrameworkSettings::GetKeyboardBindings(),
					&FrameworkSettings::SettingsSnapshot::ToggleKey },
				InputSetting{ "Toggle mode (gamepad)", "##GamePadToggleMode",
					&FrameworkSettings::SettingsSnapshot::ModeGamePad, "Toggle key (gamepad)",
					"##GamePadToggleKey", FrameworkSettings::GetGamePadBindings(),
					&FrameworkSettings::SettingsSnapshot::ToggleKeyGamePad }
			};
			for (const auto& input : inputs) {
				ImGui::Separator();
				changed = RenderToggleMode(
					input.ModeLabel, input.ModeID, edited.*input.Mode) || changed;
				changed = RenderBinding(
					input.KeyLabel, input.KeyID, input.Bindings, edited.*input.Key) || changed;
			}

			if (changed || themeChanged) {
				if (changed) {
					FrameworkSettings::RestoreSnapshot(edited);
				}
				ApplyRuntimeSettings();
				saveFailed = !SaveOrRestore(
					settingsBeforeRender, false, themeLoadFailed);
			}

			ImGui::Spacing();
			if (ImGui::Button("Reset to defaults")) {
				const auto settingsBeforeReset = FrameworkSettings::CaptureSnapshot();
				FrameworkSettings::ResetDefaults();
				fontSettingsInvalid = !FontManager::RequestAtlasRebuild(
					FrameworkSettings::GetFontSettings());
				fontSettingsRefreshRequested = true;
				themeLoadFailed = !ThemeManager::QueueConfiguredTheme();
				ApplyRuntimeSettings();
				saveFailed = !SaveOrRestore(
					settingsBeforeReset, true, themeLoadFailed);
			}

			if (themeLoadFailed) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F },
					"Could not load the selected theme");
			}
			if (saveFailed) {
				ImGui::TextColored(
					ImVec4{ 1.0F, 0.35F, 0.35F, 1.0F },
					"Could not save Data\\SFSE\\Plugins\\SFSEMenuFramework.ini");
			} else {
				ImGui::TextDisabled(
					"Settings file: Data\\SFSE\\Plugins\\SFSEMenuFramework.ini");
			}
		}
	}

	void Open() noexcept
	{
		isOpen = true;
		focusRequested = true;
	}

	void Close() noexcept
	{
		isOpen = false;
		focusRequested = false;
	}

	void ResetPlacement() noexcept
	{
		resetPlacement = true;
	}

	void Render()
	{
		if (!isOpen) {
			return;
		}

		const auto* viewport = ImGui::GetMainViewport();
		ApplyWindowPlacement(*viewport, 0.4F, 0.4F, resetPlacement);
		if (focusRequested) {
			ImGui::SetNextWindowFocus();
			focusRequested = false;
		}

		constexpr ImGuiWindowFlags windowFlags =
			ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_MenuBar |
			ImGuiWindowFlags_NoTitleBar;
		const bool drawContents = ImGui::Begin(WINDOW_ID, nullptr, windowFlags);
		if (drawContents && ImGui::BeginMenuBar()) {
			ImGui::TextUnformatted("Settings");
			if (RenderCloseButton()) {
				isOpen = false;
			}
			ImGui::EndMenuBar();
		}

		if (drawContents) {
			const float windowWidth = ImGui::GetContentRegionAvail().x;
			const float contentWidth = windowWidth * 0.8F;
			const float offset = (windowWidth - contentWidth) * 0.5F;
			if (offset > 0.0F) {
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
			}
			ImGui::BeginGroup();
			ImGui::PushItemWidth(contentWidth);
			RenderFrameworkSettings();
			ImGui::PopItemWidth();
			ImGui::EndGroup();
		}
		ImGui::End();
	}
}
