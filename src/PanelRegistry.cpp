#include "PanelRegistry.h"

#include <imgui.h>

#include <Windows.h>

#include <bit>
#include <cstring>
#include <mutex>
#include <new>
#include <system_error>

namespace SFSEMenuFramework
{
	namespace
	{
		constexpr std::size_t maximumPanelCount = 1024;

		struct Registry final
		{
			std::mutex                     Mutex;
			PanelRegistry::Snapshot         Panels;
			Model::PanelHandle              NextHandle{ 1 };
		};

		[[nodiscard]] Registry& GetRegistry()
		{
			static auto* registry = new Registry();
			return *registry;
		}

		[[nodiscard]] bool IsValidText(
			const Model::StringView& a_text,
			std::uint32_t            a_maximumLength) noexcept
		{
			return a_text.Data &&
			       a_text.Size > 0 &&
			       a_text.Size <= a_maximumLength &&
			       !std::memchr(a_text.Data, '\0', a_text.Size);
		}

		[[nodiscard]] bool HasMatchingImGuiLayout(
			const Model::ImGuiLayout& a_layout) noexcept
		{
			return a_layout.StructureSize >= sizeof(Model::ImGuiLayout) &&
			       a_layout.VersionNumber == IMGUI_VERSION_NUM &&
			       a_layout.IoSize == sizeof(ImGuiIO) &&
			       a_layout.StyleSize == sizeof(ImGuiStyle) &&
			       a_layout.Vec2Size == sizeof(ImVec2) &&
			       a_layout.Vec4Size == sizeof(ImVec4) &&
			       a_layout.DrawVertSize == sizeof(ImDrawVert) &&
			       a_layout.DrawIdxSize == sizeof(ImDrawIdx);
		}

		[[nodiscard]] bool IsExecutableImageAddress(
			Model::PanelRenderFunction a_function,
			void*&                     a_ownerModule) noexcept
		{
			static_assert(sizeof(a_function) == sizeof(const void*));
			const auto address = std::bit_cast<const void*>(a_function);

			MEMORY_BASIC_INFORMATION information{};
			if (::VirtualQuery(address, &information, sizeof(information)) !=
					sizeof(information) ||
				information.State != MEM_COMMIT ||
				information.Type != MEM_IMAGE ||
				!information.AllocationBase) {
				return false;
			}

			const auto protection = information.Protect & 0xFF;
			switch (protection) {
			case PAGE_EXECUTE:
			case PAGE_EXECUTE_READ:
			case PAGE_EXECUTE_READWRITE:
			case PAGE_EXECUTE_WRITECOPY:
				a_ownerModule = information.AllocationBase;
				return true;
			default:
				return false;
			}
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
			!IsValidText(
				a_registration->Id,
				Model::MAXIMUM_PANEL_ID_LENGTH) ||
			!IsValidText(
				a_registration->Section,
				Model::MAXIMUM_PANEL_TEXT_LENGTH) ||
			!IsValidText(
				a_registration->Title,
				Model::MAXIMUM_PANEL_TEXT_LENGTH)) {
			return Model::RegistrationResult::InvalidArgument;
		}
		if (!HasMatchingImGuiLayout(a_registration->ImGui)) {
			return Model::RegistrationResult::ImGuiMismatch;
		}

		void* ownerModule{};
		if (!IsExecutableImageAddress(a_registration->Render, ownerModule)) {
			return Model::RegistrationResult::InvalidArgument;
		}

		try {
			auto panel = std::make_shared<Panel>();
			panel->OwnerModule = ownerModule;
			panel->Id.assign(
				a_registration->Id.Data,
				a_registration->Id.Size);
			panel->Section.assign(
				a_registration->Section.Data,
				a_registration->Section.Size);
			panel->Title.assign(
				a_registration->Title.Data,
				a_registration->Title.Size);
			panel->Render = a_registration->Render;
			panel->UserData = a_registration->UserData;

			auto& registry = GetRegistry();
			std::scoped_lock lock{ registry.Mutex };
			if (registry.Panels.size() >= maximumPanelCount) {
				return Model::RegistrationResult::RegistryFull;
			}
			for (const auto& registered : registry.Panels) {
				if (registered->OwnerModule == panel->OwnerModule &&
					registered->Id == panel->Id) {
					return Model::RegistrationResult::DuplicateId;
				}
			}

			panel->Handle = registry.NextHandle++;
			registry.Panels.emplace_back(std::move(panel));
			if (a_handle) {
				*a_handle = registry.Panels.back()->Handle;
			}
			return Model::RegistrationResult::Success;
		} catch (const std::bad_alloc&) {
			return Model::RegistrationResult::OutOfMemory;
		} catch (const std::system_error&) {
			return Model::RegistrationResult::InternalError;
		}
	}

	PanelRegistry::Snapshot PanelRegistry::GetSnapshot()
	{
		auto& registry = GetRegistry();
		std::scoped_lock lock{ registry.Mutex };

		Snapshot snapshot;
		snapshot.reserve(registry.Panels.size());
		for (const auto& panel : registry.Panels) {
			if (panel->Enabled.load(std::memory_order_acquire)) {
				snapshot.push_back(panel);
			}
		}
		return snapshot;
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
			logger::error(
				"Disabled panel '{} / {}' after its render callback failed",
				a_panel->Section,
				a_panel->Title);
		} else {
			logger::info(
				"Panel '{} / {}' disabled itself",
				a_panel->Section,
				a_panel->Title);
		}
	}
}
