#pragma once

#include <SFSEMenuFramework/API.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace SFSEMenuFramework
{
	class PanelRegistry final
	{
	public:
		struct Panel final
		{
			Model::PanelHandle         Handle{ 0 };
			void*                      OwnerModule{ nullptr };
			std::string                Id;
			std::string                Section;
			std::string                Title;
			Model::PanelRenderFunction Render{ nullptr };
			void*                      UserData{ nullptr };
			std::atomic<bool>          Enabled{ true };
		};

		using PanelPointer = std::shared_ptr<Panel>;
		using Snapshot = std::vector<PanelPointer>;
		using SnapshotPointer = std::shared_ptr<const Snapshot>;

		[[nodiscard]] static Model::RegistrationResult Register(
			const Model::PanelRegistration* a_registration,
			Model::PanelHandle*              a_handle) noexcept;
		[[nodiscard]] static SnapshotPointer GetSnapshot() noexcept;
		static void Render(
			const PanelPointer&         a_panel,
			const Model::RenderContext& a_context);
	};
}
