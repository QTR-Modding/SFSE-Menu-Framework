#include "MenuOwnership.h"

#include "Win32Platform.h"
#include "WindowManager.h"

#include <atomic>
#include <limits>

namespace SFSEMenuFramework::MenuOwnership
{
	namespace
	{
		// The balanced cursor/pause edges and the proven control masks are adapted
		// from OSF UI by ozooma10 at commit 14b7565bbc7689b07fdccdb74525b9505f9f0dd6.
		// OSF UI is GPL-3.0 with Modding and GPL-3.0 Linking Exceptions.
		constexpr RE::USER_EVENT_FLAG userEventsToDisable =
			RE::USER_EVENT_FLAG::Movement |
			RE::USER_EVENT_FLAG::Looking |
			RE::USER_EVENT_FLAG::Fighting |
			RE::USER_EVENT_FLAG::Sneaking |
			RE::USER_EVENT_FLAG::Activation |
			RE::USER_EVENT_FLAG::POVSwitch |
			RE::USER_EVENT_FLAG::Menu |
			RE::USER_EVENT_FLAG::WheelZoom;

		constexpr RE::OTHER_EVENT_FLAG otherEventsToDisable =
			RE::OTHER_EVENT_FLAG::Activate |
			RE::OTHER_EVENT_FLAG::VATS |
			RE::OTHER_EVENT_FLAG::Favorites |
			RE::OTHER_EVENT_FLAG::Running |
			RE::OTHER_EVENT_FLAG::Sprinting |
			RE::OTHER_EVENT_FLAG::FastTravel |
			RE::OTHER_EVENT_FLAG::GravJump |
			RE::OTHER_EVENT_FLAG::Takeoff |
			RE::OTHER_EVENT_FLAG::HandScanner |
			RE::OTHER_EVENT_FLAG::Journal |
			RE::OTHER_EVENT_FLAG::Inventory |
			RE::OTHER_EVENT_FLAG::FarTravel;

		struct OwnershipState final
		{
			RE::BSInputEnableManager* InputManager{ nullptr };
			RE::BSInputEnableLayer*   InputLayer{ nullptr };
			RE::MenuCursor*           CursorOwner{ nullptr };
			RE::UI*                   PauseOwner{ nullptr };
			RE::UIBlurManager*        BlurOwner{ nullptr };
			bool                      LayerDisabled{ false };
			bool                      CursorOwned{ false };
			bool                      PauseOwned{ false };
			bool                      BlurOwned{ false };
			bool                      Faulted{ false };
			bool                      MissingInputManagerWarned{ false };
			bool                      MissingCursorWarned{ false };
			bool                      MissingUIWarned{ false };
			bool                      MissingBlurManagerWarned{ false };
			bool                      AllocationFailureWarned{ false };
			bool                      CursorOverflowWarned{ false };
			bool                      OwnerMismatchWarned{ false };
			bool                      ReleaseLayerWarned{ false };
			bool                      ReleaseCursorWarned{ false };
			bool                      ReleasePauseWarned{ false };
			bool                      ReleaseBlurWarned{ false };
		};

		std::atomic<LifecycleState>   lifecycleState{ LifecycleState::Uninstalled };
		std::atomic<InputDisposition> inputDisposition{ InputDisposition::PassThrough };
		std::atomic_flag              wrongThreadLogged{};
		std::atomic_flag              lifecycleInstalled{};

		[[nodiscard]] OwnershipState& GetState()
		{
			static auto* state = new OwnershipState();
			return *state;
		}

		[[nodiscard]] const RE::BSFixedString& PauseSourceName()
		{
			// The engine string table may already be tearing down at DLL detach.
			static auto* const name = new RE::BSFixedString("SFSEMenuFramework");
			return *name;
		}

		void PublishState(LifecycleState a_state, InputDisposition a_disposition) noexcept
		{
			inputDisposition.store(a_disposition, std::memory_order_release);
			lifecycleState.store(a_state, std::memory_order_release);
		}

		[[nodiscard]] bool HasAnyActiveOwnership(const OwnershipState& a_state) noexcept
		{
			return a_state.LayerDisabled || a_state.CursorOwned || a_state.PauseOwned ||
			       a_state.BlurOwned;
		}

		[[nodiscard]] bool HasCoreOwnership(const OwnershipState& a_state) noexcept
		{
			return a_state.InputLayer && a_state.LayerDisabled && a_state.CursorOwned;
		}

		[[nodiscard]] bool HasAnyCoreOwnership(const OwnershipState& a_state) noexcept
		{
			return a_state.LayerDisabled || a_state.CursorOwned;
		}

		[[nodiscard]] InputDisposition ReleaseDisposition(
			const OwnershipState& a_state) noexcept
		{
			return a_state.LayerDisabled ?
				InputDisposition::Suppress :
				InputDisposition::PassThrough;
		}

		[[nodiscard]] bool WantsInputOwnership() noexcept
		{
			const auto* mainWindow = WindowManager::GetMainWindow();
			return mainWindow &&
			       mainWindow->IsOpen.load(std::memory_order_acquire) &&
			       mainWindow->BlockUserInput.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool WantsWindowOpen() noexcept
		{
			const auto* mainWindow = WindowManager::GetMainWindow();
			return mainWindow && mainWindow->IsOpen.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool WantsPauseOwnership(const ReconcileContext& a_context) noexcept
		{
			const auto* mainWindow = WindowManager::GetMainWindow();
			return a_context.PauseAllowed && mainWindow &&
			       mainWindow->IsOpen.load(std::memory_order_acquire) &&
			       mainWindow->PauseGame.load(std::memory_order_acquire);
		}

		[[nodiscard]] bool WantsBlurOwnership() noexcept
		{
			const auto* mainWindow = WindowManager::GetMainWindow();
			return mainWindow &&
			       mainWindow->IsOpen.load(std::memory_order_acquire) &&
			       mainWindow->BlurBackground.load(std::memory_order_acquire);
		}

		void MarkFaulted(OwnershipState& a_state, const char* a_reason)
		{
			a_state.Faulted = true;
			if (!a_state.OwnerMismatchWarned) {
				a_state.OwnerMismatchWarned = true;
				logger::critical("Menu ownership faulted: {}", a_reason);
			}
			static_cast<void>(WindowManager::SetMainWindowOpen(false));
			PublishState(LifecycleState::Faulted, ReleaseDisposition(a_state));
		}

		[[nodiscard]] bool ValidateOwners(OwnershipState& a_state)
		{
			bool valid = true;
			if (a_state.InputLayer) {
				if (auto* manager = RE::BSInputEnableManager::GetSingleton();
					manager && manager != a_state.InputManager) {
					a_state.InputManager = nullptr;
					a_state.InputLayer = nullptr;
					a_state.LayerDisabled = false;
					MarkFaulted(a_state, "the BSInputEnableManager singleton changed");
					valid = false;
				}
			}
			if (a_state.CursorOwned) {
				if (auto* cursor = RE::MenuCursor::GetSingleton();
					cursor && cursor != a_state.CursorOwner) {
					a_state.CursorOwner = nullptr;
					a_state.CursorOwned = false;
					MarkFaulted(
						a_state,
						"the MenuCursor singleton changed while its reference was owned");
					valid = false;
				}
			}
			if (a_state.PauseOwned) {
				if (auto* ui = RE::UI::GetSingleton(); ui && ui != a_state.PauseOwner) {
					a_state.PauseOwner = nullptr;
					a_state.PauseOwned = false;
					MarkFaulted(
						a_state,
						"the UI singleton changed while its pause reference was owned");
					valid = false;
				}
			}
			if (a_state.BlurOwned) {
				if (auto* blur = RE::UIBlurManager::GetSingleton();
					blur && blur != a_state.BlurOwner) {
					a_state.BlurOwner = nullptr;
					a_state.BlurOwned = false;
					MarkFaulted(
						a_state,
						"the UIBlurManager singleton changed while its reference was owned");
					valid = false;
				}
			}
			return valid;
		}

		[[nodiscard]] bool EnsureRetainedLayer(OwnershipState& a_state)
		{
			auto* inputManager = RE::BSInputEnableManager::GetSingleton();
			if (!inputManager) {
				if (!a_state.MissingInputManagerWarned) {
					a_state.MissingInputManagerWarned = true;
					logger::warn("Menu ownership deferred: the input manager is not ready");
				}
				return false;
			}
			if (a_state.InputLayer) {
				if (inputManager != a_state.InputManager) {
					a_state.InputManager = nullptr;
					a_state.InputLayer = nullptr;
					a_state.LayerDisabled = false;
					MarkFaulted(
						a_state,
						"the retained input layer no longer belongs to the current manager");
					return false;
				}
				return true;
			}

			RE::BSInputEnableLayer* inputLayer{};
			if (!inputManager->AllocateNewLayer(&inputLayer, "SFSE Menu Framework") || !inputLayer) {
				if (!a_state.AllocationFailureWarned) {
					a_state.AllocationFailureWarned = true;
					logger::error("Menu ownership deferred: failed to allocate an input-enable layer");
				}
				return false;
			}

			a_state.InputManager = inputManager;
			a_state.InputLayer = inputLayer;
			a_state.MissingInputManagerWarned = false;
			a_state.AllocationFailureWarned = false;
			logger::info(
				"Menu input layer {} retained for the process lifetime",
				inputLayer->GetLayerID());
			return true;
		}

		[[nodiscard]] bool SetLayerDisabled(OwnershipState& a_state, bool a_disabled)
		{
			if (a_state.LayerDisabled == a_disabled) {
				return true;
			}
			if (!a_state.InputLayer) {
				return !a_disabled;
			}

			auto* manager = RE::BSInputEnableManager::GetSingleton();
			if (!manager) {
				if (!a_state.ReleaseLayerWarned) {
					a_state.ReleaseLayerWarned = true;
					logger::warn("Input-layer transition deferred: the input manager is unavailable");
				}
				return false;
			}
			if (manager != a_state.InputManager) {
				a_state.InputManager = nullptr;
				a_state.InputLayer = nullptr;
				a_state.LayerDisabled = false;
				MarkFaulted(
					a_state,
					"the retained input layer no longer belongs to the current manager");
				return false;
			}

			a_state.InputLayer->EnableUserEvent(userEventsToDisable, !a_disabled);
			a_state.InputLayer->EnableOtherEvent(otherEventsToDisable, !a_disabled);
			a_state.LayerDisabled = a_disabled;
			a_state.ReleaseLayerWarned = false;
			return true;
		}

		[[nodiscard]] bool AcquireCursor(OwnershipState& a_state)
		{
			if (a_state.CursorOwned) {
				auto* cursor = RE::MenuCursor::GetSingleton();
				if (!cursor) {
					if (!a_state.MissingCursorWarned) {
						a_state.MissingCursorWarned = true;
						logger::warn(
							"Menu ownership deferred: retained cursor ownership is pending release");
					}
					return false;
				}
				if (cursor != a_state.CursorOwner) {
					a_state.CursorOwner = nullptr;
					a_state.CursorOwned = false;
					MarkFaulted(
						a_state,
						"the retained MenuCursor owner is no longer the current singleton");
					return false;
				}
				return true;
			}
			auto* cursor = RE::MenuCursor::GetSingleton();
			if (!cursor) {
				if (!a_state.MissingCursorWarned) {
					a_state.MissingCursorWarned = true;
					logger::warn("Menu ownership deferred: the menu cursor is not ready");
				}
				return false;
			}
			if (cursor->freeCursorRefCount == (std::numeric_limits<std::uint32_t>::max)()) {
				if (!a_state.CursorOverflowWarned) {
					a_state.CursorOverflowWarned = true;
					logger::error("Menu ownership rejected: the free-cursor reference count is full");
				}
				return false;
			}

			++cursor->freeCursorRefCount;
			a_state.CursorOwner = cursor;
			a_state.CursorOwned = true;
			a_state.MissingCursorWarned = false;
			return true;
		}

		[[nodiscard]] bool ReleaseCursor(OwnershipState& a_state)
		{
			if (!a_state.CursorOwned) {
				return true;
			}
			auto* cursor = RE::MenuCursor::GetSingleton();
			if (!cursor) {
				if (!a_state.ReleaseCursorWarned) {
					a_state.ReleaseCursorWarned = true;
					logger::warn("Free-cursor release deferred: MenuCursor is unavailable");
				}
				return false;
			}
			if (cursor != a_state.CursorOwner) {
				a_state.CursorOwner = nullptr;
				a_state.CursorOwned = false;
				MarkFaulted(a_state, "the owned MenuCursor is no longer the current singleton");
				return false;
			}

			if (cursor->freeCursorRefCount > 0) {
				--cursor->freeCursorRefCount;
			} else if (!a_state.ReleaseCursorWarned) {
				a_state.ReleaseCursorWarned = true;
				logger::warn("The free-cursor reference count was already zero during release");
			}
			a_state.CursorOwner = nullptr;
			a_state.CursorOwned = false;
			a_state.ReleaseCursorWarned = false;
			return true;
		}

		[[nodiscard]] bool SetPauseOwned(OwnershipState& a_state, bool a_owned)
		{
			if (a_state.PauseOwned == a_owned) {
				return true;
			}

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				if (a_owned) {
					if (!a_state.MissingUIWarned) {
						a_state.MissingUIWarned = true;
						logger::warn("Menu pause deferred: UI is unavailable");
					}
				} else if (!a_state.ReleasePauseWarned) {
					a_state.ReleasePauseWarned = true;
					logger::warn("Menu pause release deferred: UI is unavailable");
				}
				return false;
			}
			if (!a_owned && ui != a_state.PauseOwner) {
				a_state.PauseOwner = nullptr;
				a_state.PauseOwned = false;
				MarkFaulted(
					a_state,
					"the owned UI pause reference is no longer on the current singleton");
				return false;
			}

			ui->ModifyMenuPauseCounter(PauseSourceName(), a_owned);
			a_state.PauseOwner = a_owned ? ui : nullptr;
			a_state.PauseOwned = a_owned;
			a_state.MissingUIWarned = false;
			a_state.ReleasePauseWarned = false;
			return true;
		}

		[[nodiscard]] bool SetBlurOwned(OwnershipState& a_state, bool a_owned)
		{
			if (a_state.BlurOwned == a_owned) {
				return true;
			}

			auto* blur = RE::UIBlurManager::GetSingleton();
			if (!blur) {
				if (a_owned) {
					if (!a_state.MissingBlurManagerWarned) {
						a_state.MissingBlurManagerWarned = true;
						logger::warn("Menu background blur deferred: UIBlurManager is unavailable");
					}
				} else if (!a_state.ReleaseBlurWarned) {
					a_state.ReleaseBlurWarned = true;
					logger::warn("Menu background blur release deferred: UIBlurManager is unavailable");
				}
				return false;
			}
			if (!a_owned && blur != a_state.BlurOwner) {
				a_state.BlurOwner = nullptr;
				a_state.BlurOwned = false;
				MarkFaulted(
					a_state,
					"the owned blur reference is no longer on the current singleton");
				return false;
			}

			// Balanced blur ownership follows SKSE Menu Framework 3 at commit
			// 928e01ab459822a8d233ab99f0419ea1de23c775 (GPL-3.0), src/GameLock.cpp.
			if (a_owned) {
				blur->IncrementBlurCount();
			} else {
				blur->DecrementBlurCount();
			}
			a_state.BlurOwner = a_owned ? blur : nullptr;
			a_state.BlurOwned = a_owned;
			a_state.MissingBlurManagerWarned = false;
			a_state.ReleaseBlurWarned = false;
			return true;
		}

		[[nodiscard]] bool ReleaseCoreOwnership(OwnershipState& a_state)
		{
			if (!HasAnyCoreOwnership(a_state)) {
				inputDisposition.store(
					InputDisposition::PassThrough,
					std::memory_order_release);
				return true;
			}

			inputDisposition.store(ReleaseDisposition(a_state), std::memory_order_release);
			const bool cursorReleased = ReleaseCursor(a_state);
			const bool layerReleased = SetLayerDisabled(a_state, false);
			if (a_state.Faulted) {
				inputDisposition.store(
					ReleaseDisposition(a_state),
					std::memory_order_release);
				return false;
			}
			const bool released = cursorReleased && layerReleased &&
			                      !HasAnyCoreOwnership(a_state);
			inputDisposition.store(
				ReleaseDisposition(a_state),
				std::memory_order_release);
			return released;
		}

		[[nodiscard]] bool ReleaseAllOwnership(OwnershipState& a_state)
		{
			PublishState(LifecycleState::ReleasePending, ReleaseDisposition(a_state));

			const bool blurReleased = SetBlurOwned(a_state, false);
			const bool pauseReleased = SetPauseOwned(a_state, false);
			const bool coreReleased = ReleaseCoreOwnership(a_state);

			if (a_state.Faulted) {
				return false;
			}
			if (!blurReleased || !pauseReleased || !coreReleased ||
				HasAnyActiveOwnership(a_state)) {
				PublishState(LifecycleState::ReleasePending, ReleaseDisposition(a_state));
				return false;
			}

			PublishState(LifecycleState::Dormant, InputDisposition::PassThrough);
			logger::info("Menu ownership released; retained input layer remains dormant");
			return true;
		}

		[[nodiscard]] bool AcquireCoreOwnership(OwnershipState& a_state)
		{
			PublishState(LifecycleState::Arming, InputDisposition::Suppress);

			if (!EnsureRetainedLayer(a_state) || !SetLayerDisabled(a_state, true)) {
				if (!a_state.Faulted) {
					const auto disposition = ReleaseDisposition(a_state);
					PublishState(
						disposition == InputDisposition::Suppress ?
							LifecycleState::ReleasePending :
							LifecycleState::AwaitingLayer,
						disposition);
				}
				return false;
			}
			if (!AcquireCursor(a_state)) {
				static_cast<void>(SetLayerDisabled(a_state, false));
				if (a_state.Faulted) {
					return false;
				}
				const bool cleanupPending = HasAnyActiveOwnership(a_state);
				PublishState(
					cleanupPending ?
						LifecycleState::ReleasePending :
						LifecycleState::Dormant,
					ReleaseDisposition(a_state));
				return false;
			}

			logger::info(
				"Menu core ownership acquired (retained input layer {}, free-cursor references {})",
				a_state.InputLayer->GetLayerID(),
				a_state.CursorOwner->freeCursorRefCount);
			return true;
		}

		void ReconcileOptionalEffects(
			OwnershipState& a_state,
			bool            a_pauseWanted,
			bool            a_blurWanted)
		{
			static_cast<void>(SetPauseOwned(a_state, a_pauseWanted));
			if (!a_state.Faulted) {
				static_cast<void>(SetBlurOwned(a_state, a_blurWanted));
			}
		}

	}

	void Install()
	{
		if (lifecycleInstalled.test_and_set(std::memory_order_acq_rel)) {
			logger::warn("Menu input lifecycle is already installed");
			return;
		}

		PublishState(LifecycleState::AwaitingLayer, InputDisposition::PassThrough);
		logger::info("Engine menu-ownership lifecycle installed");
	}

	void ReconcileOnHostWindowThread(const ReconcileContext& a_context)
	{
		if (!lifecycleInstalled.test(std::memory_order_acquire)) {
			return;
		}
		if (!Win32Platform::IsCurrentThreadHostWindowThread()) {
			if (!wrongThreadLogged.test_and_set(std::memory_order_relaxed)) {
				logger::critical(
					"Menu ownership reconciliation rejected: caller does not own the Starfield window");
			}
			return;
		}

		auto& state = GetState();
		const bool ownersValid = ValidateOwners(state);
		if (state.Faulted || !ownersValid) {
			if (HasAnyActiveOwnership(state)) {
				static_cast<void>(ReleaseAllOwnership(state));
			}
			PublishState(LifecycleState::Faulted, ReleaseDisposition(state));
			return;
		}

		const bool windowOpen = WantsWindowOpen();
		const bool wantsInput = WantsInputOwnership();
		const bool wantsPause = WantsPauseOwnership(a_context);
		const bool wantsBlur = WantsBlurOwnership();
		if (a_context.Availability == HostAvailability::Transient) {
			if (HasAnyActiveOwnership(state)) {
				if (!ReleaseAllOwnership(state)) {
					return;
				}
			}
			PublishState(LifecycleState::Suspended, InputDisposition::PassThrough);
			return;
		}

		if (!windowOpen || a_context.Availability == HostAvailability::Unavailable) {
			if (HasAnyActiveOwnership(state)) {
				if (!ReleaseAllOwnership(state)) {
					return;
				}
			} else if (!state.InputLayer &&
				a_context.Availability == HostAvailability::Interactive) {
				static_cast<void>(EnsureRetainedLayer(state));
			}
			if (!state.Faulted) {
				PublishState(
					state.InputLayer ? LifecycleState::Dormant : LifecycleState::AwaitingLayer,
					InputDisposition::PassThrough);
			}
			return;
		}

		if (!state.InputLayer) {
			static_cast<void>(EnsureRetainedLayer(state));
			if (state.Faulted) {
				return;
			}
		}

		if (wantsInput && HasAnyCoreOwnership(state) && !HasCoreOwnership(state)) {
			if (!ReleaseCoreOwnership(state)) {
				if (state.Faulted) {
					if (HasAnyActiveOwnership(state)) {
						static_cast<void>(ReleaseAllOwnership(state));
					}
					PublishState(LifecycleState::Faulted, ReleaseDisposition(state));
				}
				return;
			}
		}

		if (wantsInput) {
			if (!HasCoreOwnership(state) && !AcquireCoreOwnership(state)) {
				if (state.Faulted) {
					if (HasAnyActiveOwnership(state)) {
						static_cast<void>(ReleaseAllOwnership(state));
					}
					PublishState(LifecycleState::Faulted, ReleaseDisposition(state));
				}
				return;
			}
		} else if (!ReleaseCoreOwnership(state)) {
			return;
		}

		ReconcileOptionalEffects(state, wantsPause, wantsBlur);
		if (state.Faulted) {
			if (HasAnyActiveOwnership(state)) {
				static_cast<void>(ReleaseAllOwnership(state));
			}
			PublishState(LifecycleState::Faulted, ReleaseDisposition(state));
			return;
		}

		PublishState(
			LifecycleState::Active,
			wantsInput && HasCoreOwnership(state) ?
				InputDisposition::RouteToMenu :
				InputDisposition::PassThrough);
	}

	void ReleaseOnHostWindowThread()
	{
		ReconcileOnHostWindowThread({
			.Availability = HostAvailability::Unavailable,
			.PauseAllowed = false,
		});
	}

	LifecycleState GetLifecycleState() noexcept
	{
		return lifecycleState.load(std::memory_order_acquire);
	}

	InputDisposition GetInputDisposition() noexcept
	{
		return inputDisposition.load(std::memory_order_acquire);
	}
}
