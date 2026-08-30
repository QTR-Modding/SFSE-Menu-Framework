#include "MenuLifecycle.h"

#include "D3D12Renderer.h"
#include "FrameworkSettings.h"
#include "InputCapture.h"
#include "McpWindow.h"
#include "MenuOwnership.h"
#include "Win32Platform.h"
#include "WindowManager.h"

#include <atomic>

namespace SFSEMenuFramework::MenuLifecycle
{
	namespace
	{
		std::atomic_flag installed{};
		std::atomic<bool> installReady{ false };

		struct HostClassification final
		{
			MenuOwnership::HostAvailability Availability{
				MenuOwnership::HostAvailability::Unavailable
			};
			bool RenderEnabled{ false };
		};

		[[nodiscard]] HostClassification ClassifyHost() noexcept
		{
			using HostAvailability = MenuOwnership::HostAvailability;
			if (!Win32Platform::IsInitialized()) {
				return { .Availability = HostAvailability::Unavailable };
			}
			if (!Win32Platform::IsHostWindowUsable()) {
				return { .Availability = HostAvailability::Transient };
			}

			const auto* ui = RE::UI::GetSingleton();
			static auto* const loadingMenu = new RE::BSFixedString("LoadingMenu");
			if (!ui || ui->IsMenuOpen(*loadingMenu)) {
				return { .Availability = HostAvailability::Transient };
			}

			const auto generation = WindowManager::GetMainWindowOpenGeneration();
			if (generation != 0 &&
				(!D3D12Renderer::HasRecentMainWindowFrame(generation) ||
				 !WindowManager::IsMainWindowOpenGeneration(generation))) {
				return {
					.Availability = HostAvailability::Transient,
					.RenderEnabled = true,
				};
			}
			return {
				.Availability = HostAvailability::Interactive,
				.RenderEnabled = true,
			};
		}

		void ReconcileHostWindow() noexcept
		{
			InputCapture::FlushDiagnostics();

			using InputDisposition = MenuOwnership::InputDisposition;
			const auto host = ClassifyHost();
			const auto availability = host.Availability;
			WindowManager::SetMainWindowRenderEnabled(host.RenderEnabled);
			MenuOwnership::ReconcileOnHostWindowThread({
				.Availability = availability,
				.PauseAllowed =
					availability == MenuOwnership::HostAvailability::Interactive,
			});

			const auto disposition = MenuOwnership::GetInputDisposition();
			const bool suppressNativeInput =
				disposition != InputDisposition::PassThrough;
			InputCapture::SetModal(suppressNativeInput);
			D3D12Renderer::SetPlatformInputEnabled(
				disposition == InputDisposition::RouteToMenu);

			const auto* mainWindow = WindowManager::GetMainWindow();
			const bool blockingWindowOpen = mainWindow &&
				mainWindow->IsOpen.load(std::memory_order_acquire) &&
				mainWindow->BlockUserInput.load(std::memory_order_acquire);
			if (availability == MenuOwnership::HostAvailability::Interactive &&
				blockingWindowOpen && disposition != InputDisposition::RouteToMenu) {
				logger::critical(
					"Blocking menu ownership could not be completed; closing the Mod Control Panel");
				static_cast<void>(WindowManager::SetMainWindowOpen(false));
				D3D12Renderer::SetPlatformInputEnabled(false);
				MenuOwnership::ReleaseOnHostWindowThread();
				InputCapture::SetModal(
					MenuOwnership::GetInputDisposition() != InputDisposition::PassThrough);
				return;
			}

			if (suppressNativeInput && !InputCapture::IsModal()) {
				logger::critical(
					"Menu ownership was acquired without operational native capture; closing the Mod Control Panel");
				static_cast<void>(WindowManager::SetMainWindowOpen(false));
				D3D12Renderer::SetPlatformInputEnabled(false);
				MenuOwnership::ReleaseOnHostWindowThread();
			}
		}
	}

	bool Install(const SFSE::TaskInterface& a_taskInterface)
	{
		if (installed.test_and_set(std::memory_order_acq_rel)) {
			return installReady.load(std::memory_order_acquire);
		}

		if (!FrameworkSettings::Load()) {
			logger::warn(
				"One or more SFSEMenuFramework.ini values were invalid; defaults were applied to those fields");
		}

		if (!InputCapture::Install()) {
			logger::critical(
				"Failed to install native input capture; the Mod Control Panel will remain unavailable");
			return false;
		}
		if (!McpWindow::Install()) {
			logger::critical(
				"Failed to register the built-in Mod Control Panel window");
			return false;
		}
		Win32Platform::SetHostWindowCallback(&ReconcileHostWindow);
		MenuOwnership::Install(a_taskInterface);
		InputCapture::ArmFunctionalCapture();
		installReady.store(true, std::memory_order_release);
		logger::info("Mod Control Panel ready; use the configured hotkey to open it");
		return true;
	}
}
