#pragma once

#include <SFSEMenuFramework/API.h>

#include <atomic>
#include <cstdint>

namespace SFSEMenuFramework
{
	class WindowInterface final : public Model::WindowInterface
	{
	public:
		std::atomic<bool> PauseGame{ true };
		std::atomic<bool> BlurBackground{ true };
	};

	using WindowRenderFunction = void(__stdcall*)(const Model::RenderContext&);

	class WindowManager final
	{
	public:
		static WindowInterface* AddWindow(WindowRenderFunction a_renderFunction);
		[[nodiscard]] static Model::RegistrationResult RegisterWindow(
			const Model::WindowRegistration* a_registration,
			Model::WindowInterface**          a_window) noexcept;
		[[nodiscard]] static std::uint64_t RenderOpenWindows(
			const Model::RenderContext& a_context);

		static bool                          SetMainWindow(WindowInterface* a_window) noexcept;
		[[nodiscard]] static WindowInterface* GetMainWindow() noexcept;
		static void SetMainWindowRenderEnabled(bool a_enabled) noexcept;
		[[nodiscard]] static bool SetMainWindowOpen(bool a_open) noexcept;
		[[nodiscard]] static bool IsAnyWindowOpen() noexcept;
		[[nodiscard]] static bool IsAnyBlockingWindowOpened() noexcept;
		[[nodiscard]] static bool ShouldPauseGame() noexcept;
		[[nodiscard]] static bool ShouldBlurBackground() noexcept;
		static void CloseAllBlockingWindows() noexcept;
		static void SetHotkeyEnabled(bool a_enabled) noexcept;
		[[nodiscard]] static bool IsHotkeyEnabled() noexcept;
		[[nodiscard]] static std::uint64_t GetBlockingWindowOpenGeneration() noexcept;
		[[nodiscard]] static bool IsBlockingWindowOpenGeneration(
			std::uint64_t a_generation) noexcept;
		[[nodiscard]] static std::uint64_t GetMainWindowSessionGeneration() noexcept;
	};
}
