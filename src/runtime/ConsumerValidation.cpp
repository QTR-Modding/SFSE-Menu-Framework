#include "runtime/ConsumerValidation.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <Windows.h>

namespace SFSEMenuFramework::Detail
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
