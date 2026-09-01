#include "PanelRegistry.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <system_error>

namespace SFSEMenuFramework
{
	namespace Detail
	{
		bool HasMatchingImGuiLayout(
			const Model::ImGuiLayout& a_layout) noexcept
		{
			return a_layout.StructureSize >= sizeof(Model::ImGuiLayout) &&
			       a_layout.VersionNumber == IMGUI_VERSION_NUM &&
			       a_layout.SourceRevision == Model::IMGUI_SOURCE_REVISION &&
			       a_layout.ConfigurationFlags == 0 &&
			       a_layout.IoSize == sizeof(ImGuiIO) &&
			       a_layout.StyleSize == sizeof(ImGuiStyle) &&
			       a_layout.ContextSize == sizeof(ImGuiContext) &&
			       a_layout.Vec2Size == sizeof(ImVec2) &&
			       a_layout.Vec4Size == sizeof(ImVec4) &&
			       a_layout.DrawVertSize == sizeof(ImDrawVert) &&
			       a_layout.DrawIdxSize == sizeof(ImDrawIdx) &&
			       a_layout.DrawCmdSize == sizeof(ImDrawCmd) &&
			       a_layout.TextureIdSize == sizeof(ImTextureID) &&
			       a_layout.WcharSize == sizeof(ImWchar);
		}

		bool IsExecutableImageAddress(
			const void* a_address,
			void**      a_ownerModule) noexcept
		{
			MEMORY_BASIC_INFORMATION information{};
			if (::VirtualQuery(a_address, &information, sizeof(information)) !=
					sizeof(information) ||
				information.State != MEM_COMMIT ||
				information.Type != MEM_IMAGE ||
				(information.Protect & PAGE_GUARD) != 0 ||
				!information.AllocationBase) {
				return false;
			}

			const auto protection = information.Protect & 0xFF;
			const bool executable = protection == PAGE_EXECUTE ||
				protection == PAGE_EXECUTE_READ ||
				protection == PAGE_EXECUTE_READWRITE ||
				protection == PAGE_EXECUTE_WRITECOPY;
			if (executable && a_ownerModule) {
				*a_ownerModule = information.AllocationBase;
			}
			return executable;
		}
	}

	namespace
	{
		constexpr std::size_t maximumPanelCount = 1024;

		struct Registry final
		{
			std::mutex                                        Mutex;
			std::atomic<PanelRegistry::SnapshotPointer>        Panels;
			Model::PanelHandle                                 NextHandle{ 1 };
		};

		[[nodiscard]] Registry* GetRegistry() noexcept
		{
			static auto* registry = new (std::nothrow) Registry();
			return registry;
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
		if (!Detail::HasMatchingImGuiLayout(a_registration->ImGui)) {
			return Model::RegistrationResult::ImGuiMismatch;
		}

		void* ownerModule{};
		if (!Detail::IsExecutableImageFunction(
				a_registration->Render,
				&ownerModule)) {
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

			auto* registry = GetRegistry();
			if (!registry) {
				return Model::RegistrationResult::OutOfMemory;
			}
			std::scoped_lock lock{ registry->Mutex };
			const auto current = registry->Panels.load(std::memory_order_acquire);
			if (current && current->size() >= maximumPanelCount) {
				return Model::RegistrationResult::RegistryFull;
			}
			if (current) {
				for (const auto& registered : *current) {
					if (registered->OwnerModule == panel->OwnerModule &&
						registered->Id == panel->Id) {
						return Model::RegistrationResult::DuplicateId;
					}
				}
			}

			auto next = current ?
			                std::make_shared<Snapshot>(*current) :
			                std::make_shared<Snapshot>();
			panel->Handle = registry->NextHandle++;
			const auto registeredHandle = panel->Handle;
			next->emplace_back(std::move(panel));
			std::stable_sort(
				next->begin(),
				next->end(),
				[](const auto& a_left, const auto& a_right) {
					return a_left->Section < a_right->Section;
				});
			if (a_handle) {
				*a_handle = registeredHandle;
			}
			registry->Panels.store(std::move(next), std::memory_order_release);
			return Model::RegistrationResult::Success;
		} catch (const std::bad_alloc&) {
			return Model::RegistrationResult::OutOfMemory;
		} catch (const std::system_error&) {
			return Model::RegistrationResult::InternalError;
		}
	}

	PanelRegistry::SnapshotPointer PanelRegistry::GetSnapshot() noexcept
	{
		const auto* registry = GetRegistry();
		return registry ?
		           registry->Panels.load(std::memory_order_acquire) :
		           nullptr;
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
