#include "PanelRegistry.h"

namespace
{
	[[nodiscard]] SFSEMenuFramework::Model::RegistrationResult __stdcall RegisterPanel(
		const SFSEMenuFramework::Model::PanelRegistration* a_registration,
		SFSEMenuFramework::Model::PanelHandle*              a_handle) noexcept
	{
		return SFSEMenuFramework::PanelRegistry::Register(a_registration, a_handle);
	}

	const SFSEMenuFramework::Model::Interface interfaceV1{
		.StructureSize = sizeof(SFSEMenuFramework::Model::Interface),
		.Version = SFSEMenuFramework::Model::INTERFACE_VERSION,
		.RegisterPanel = &RegisterPanel
	};
}

extern "C" __declspec(dllexport)
	const SFSEMenuFramework::Model::Interface* __stdcall
	SFSEMenuFramework_QueryInterface(std::uint32_t a_requestedVersion) noexcept
{
	return a_requestedVersion == SFSEMenuFramework::Model::INTERFACE_VERSION ?
	           &interfaceV1 :
	           nullptr;
}
