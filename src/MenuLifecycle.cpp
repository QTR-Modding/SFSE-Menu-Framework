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
		std::atomic_flag earlyInstallStarted{};
		std::atomic<bool> earlyInstallReady{ false };
		std::atomic_flag postDataLoadActivationStarted{};
		std::atomic<bool> postDataLoadReady{ false };
		std::atomic_flag hostBootstrapFailureLogged{};

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

		void BootstrapHostWindow()
		{
			switch (Win32Platform::Initialize()) {
			case Win32Platform::InitializeResult::Ready:
				static_cast<void>(Win32Platform::PostHostWindowCallback());
				break;
			case Win32Platform::InitializeResult::Deferred:
				break;
			case Win32Platform::InitializeResult::Failed:
				if (!hostBootstrapFailureLogged.test_and_set(
						std::memory_order_relaxed)) {
					logger::critical(
						"Menu lifecycle could not initialize the Win32 platform");
				}
				break;
			}
		}

		void ReconcileFrameworkOnlyHostWindow() noexcept
		{
			const bool renderEnabled =
				Win32Platform::IsInitialized() &&
				Win32Platform::IsHostWindowUsable();
			WindowManager::SetMainWindowRenderEnabled(renderEnabled);

			InputCapture::SetModal(false);
			D3D12Renderer::SetPlatformInputEnabled(false);
		}

		void ReconcileHostWindow() noexcept
		{
			InputCapture::FlushDiagnostics();
			if (!postDataLoadReady.load(std::memory_order_acquire)) {
				ReconcileFrameworkOnlyHostWindow();
				return;
			}

			using InputDisposition = MenuOwnership::InputDisposition;
			const auto host = ClassifyHost();
			const auto availability = host.Availability;
			WindowManager::SetMainWindowRenderEnabled(host.RenderEnabled);
			MenuOwnership::ReconcileOnHostWindowThread({
				.Availability = availability,
				.PauseAllowed =
					availability == MenuOwnership::HostAvailability::Interactive,
			});
			// Publish full input capture only after the HWND thread has reconciled
			// any already-open startup panel with engine ownership.
			InputCapture::ArmFunctionalCapture();

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

	bool InstallEarly(const SFSE::TaskInterface& a_taskInterface)
	{
		if (earlyInstallStarted.test_and_set(std::memory_order_acq_rel)) {
			return earlyInstallReady.load(std::memory_order_acquire);
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
		a_taskInterface.AddPermanentTask(&BootstrapHostWindow);
		InputCapture::ArmKeyboardEdgeCapture();
		earlyInstallReady.store(true, std::memory_order_release);
		logger::info(
			"Mod Control Panel registered during plugin load; it can be opened and rendered once the Starfield window and renderer are ready; full interaction waits for post-data-load");
		return true;
	}

	bool ActivatePostDataLoad() noexcept
	{
		if (postDataLoadActivationStarted.test_and_set(std::memory_order_acq_rel)) {
			return postDataLoadReady.load(std::memory_order_acquire);
		}
		if (!earlyInstallReady.load(std::memory_order_acquire)) {
			return false;
		}

		MenuOwnership::Install();
		postDataLoadReady.store(true, std::memory_order_release);
		logger::info(
			"SFSE post-data-load reached; engine menu ownership can now activate");
		static_cast<void>(Win32Platform::PostHostWindowCallback());
		return true;
	}
}
