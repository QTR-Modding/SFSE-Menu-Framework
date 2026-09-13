#include "lifecycle/MenuLifecycle.h"
#include "localization/Translations.h"

#include "rendering/D3D12Renderer.h"
#include "config/FrameworkSettings.h"
#include "input/InputCapture.h"
#include "lifecycle/MenuOwnership.h"
#include "ui/McpWindow.h"
#include "platform/win32/Win32Platform.h"
#include "runtime/WindowManager.h"

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
		std::atomic_flag earlyInteractionLogged{};

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

			const auto generation = WindowManager::GetBlockingWindowOpenGeneration();
			if (generation != 0 &&
				(!D3D12Renderer::HasRecentBlockingWindowFrame(generation) ||
				 !WindowManager::IsBlockingWindowOpenGeneration(generation))) {
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

		void AbortBlockingInteraction(const char* a_reason) noexcept
		{
			logger::critical("{}; closing all blocking framework windows", a_reason);
			WindowManager::CloseAllBlockingWindows();
			static_cast<void>(D3D12Renderer::SetPlatformInputEnabled(false));
		}

		void ReconcileFrameworkOnlyHostWindow() noexcept
		{
			const bool renderEnabled =
				Win32Platform::IsHostWindowUsable();
			WindowManager::SetMainWindowRenderEnabled(renderEnabled);

			const auto generation =
				WindowManager::GetBlockingWindowOpenGeneration();
			const bool routeToMenu = renderEnabled &&
				WindowManager::IsAnyBlockingWindowOpened() &&
				generation != 0 &&
				D3D12Renderer::HasRecentBlockingWindowFrame(generation) &&
				WindowManager::IsBlockingWindowOpenGeneration(generation);
			if (!routeToMenu) {
				static_cast<void>(
					D3D12Renderer::SetPlatformInputEnabled(false));
				InputCapture::SetModal(false);
				return;
			}

			// A startup open/close batch is suppressed at its exact input edge, but
			// persistent modal capture begins only after this generation has produced
			// a visible frame. That keeps a failed or delayed renderer from trapping
			// native input behind an invisible menu.
			InputCapture::SetModal(true);
			if (!InputCapture::IsModal()) {
				AbortBlockingInteraction("Early native input capture could not be armed");
				InputCapture::SetModal(false);
				return;
			}

			if (!D3D12Renderer::SetPlatformInputEnabled(true, generation)) {
				AbortBlockingInteraction("Early relative mouse routing could not be armed");
				InputCapture::SetModal(false);
				return;
			}
			if (!earlyInteractionLogged.test_and_set(std::memory_order_relaxed)) {
				logger::info(
					"Pre-post-data-load blocking framework input activated for rendered generation {}",
					generation);
			}
		}

		void ReconcileHostWindow() noexcept
		{
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
			const auto disposition = MenuOwnership::GetInputDisposition();
			const bool suppressNativeInput =
				disposition != InputDisposition::PassThrough;
			InputCapture::SetModal(suppressNativeInput);
			const bool platformInputReady =
				D3D12Renderer::SetPlatformInputEnabled(
				disposition == InputDisposition::RouteToMenu);

			const bool blockingWindowOpen =
				WindowManager::IsAnyBlockingWindowOpened();
			if (availability == MenuOwnership::HostAvailability::Interactive &&
				blockingWindowOpen &&
				(disposition != InputDisposition::RouteToMenu || !platformInputReady)) {
				AbortBlockingInteraction(
					"Blocking menu ownership could not be completed");
				MenuOwnership::ReleaseOnHostWindowThread();
				InputCapture::SetModal(
					MenuOwnership::GetInputDisposition() != InputDisposition::PassThrough);
				return;
			}

			if (suppressNativeInput && !InputCapture::IsModal()) {
				AbortBlockingInteraction(
					"Menu ownership was acquired without operational native capture");
				MenuOwnership::ReleaseOnHostWindowThread();
			}
		}
	}

	bool InstallEarly(const SFSE::TaskInterface& a_taskInterface)
	{
		if (earlyInstallStarted.test_and_set(std::memory_order_acq_rel)) {
			return earlyInstallReady.load(std::memory_order_acquire);
		}

		if (!Translations::Load(FrameworkSettings::BuildGamePath(
			L"Data/SFSE/Plugins/SFSEMenuFrameworkStrings.json"))) {
			logger::info("Translation file missing or invalid; using English");
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
		InputCapture::ArmFunctionalCapture();
		earlyInstallReady.store(true, std::memory_order_release);
		logger::info(
			"Mod Control Panel registered during plugin load; it becomes interactive "
			"once the Starfield window and renderer produce its first visible frame");
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
