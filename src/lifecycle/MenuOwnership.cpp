#include "lifecycle/MenuOwnership.h"

#include "platform/win32/Win32Platform.h"
#include "runtime/WindowManager.h"

#include <atomic>
#include <limits>
#include <utility>

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
		};

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

		void PublishDisposition(InputDisposition a_disposition) noexcept
		{
			inputDisposition.store(a_disposition, std::memory_order_release);
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

		void MarkFaulted(OwnershipState& a_state, const char* a_reason)
		{
			if (!std::exchange(a_state.Faulted, true)) {
				logger::critical("Menu ownership faulted: {}", a_reason);
			}
			WindowManager::CloseAllBlockingWindows();
			PublishDisposition(ReleaseDisposition(a_state));
		}

		void LoseInputLayer(OwnershipState& a_state, const char* a_reason)
		{
			a_state.InputManager = nullptr;
			a_state.InputLayer = nullptr;
			a_state.LayerDisabled = false;
			MarkFaulted(a_state, a_reason);
		}

		template <class Owner>
		void ValidateOwner(
			OwnershipState& a_state,
			bool&           a_owned,
			Owner*&         a_owner,
			Owner*          a_current,
			const char*     a_reason)
		{
			if (a_owned && a_current && a_current != a_owner) {
				a_owner = nullptr;
				a_owned = false;
				MarkFaulted(a_state, a_reason);
			}
		}

		void ValidateOwners(OwnershipState& a_state)
		{
			if (a_state.InputLayer) {
				if (auto* manager = RE::BSInputEnableManager::GetSingleton();
					manager && manager != a_state.InputManager) {
					LoseInputLayer(a_state, "the BSInputEnableManager singleton changed");
				}
			}
			ValidateOwner(
				a_state,
				a_state.CursorOwned,
				a_state.CursorOwner,
				RE::MenuCursor::GetSingleton(),
				"the MenuCursor singleton changed while its reference was owned");
			ValidateOwner(
				a_state,
				a_state.PauseOwned,
				a_state.PauseOwner,
				RE::UI::GetSingleton(),
				"the UI singleton changed while its pause reference was owned");
			ValidateOwner(
				a_state,
				a_state.BlurOwned,
				a_state.BlurOwner,
				RE::UIBlurManager::GetSingleton(),
				"the UIBlurManager singleton changed while its reference was owned");
		}

		[[nodiscard]] bool EnsureRetainedLayer(OwnershipState& a_state)
		{
			auto* inputManager = RE::BSInputEnableManager::GetSingleton();
			if (!inputManager) {
				return false;
			}
			if (a_state.InputLayer) {
				if (inputManager != a_state.InputManager) {
					LoseInputLayer(a_state,
						"the retained input layer no longer belongs to the current manager");
					return false;
				}
				return true;
			}

			RE::BSInputEnableLayer* inputLayer{};
			if (!inputManager->AllocateNewLayer(&inputLayer, "SFSE Menu Framework") || !inputLayer) {
				return false;
			}

			a_state.InputManager = inputManager;
			a_state.InputLayer = inputLayer;
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
				return false;
			}
			if (manager != a_state.InputManager) {
				LoseInputLayer(a_state,
					"the retained input layer no longer belongs to the current manager");
				return false;
			}

			a_state.InputLayer->EnableUserEvent(userEventsToDisable, !a_disabled);
			a_state.InputLayer->EnableOtherEvent(otherEventsToDisable, !a_disabled);
			a_state.LayerDisabled = a_disabled;
			return true;
		}

		[[nodiscard]] bool AcquireCursor(OwnershipState& a_state)
		{
			if (a_state.CursorOwned) {
				auto* cursor = RE::MenuCursor::GetSingleton();
				if (!cursor) {
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
				return false;
			}
			if (cursor->freeCursorRefCount == (std::numeric_limits<std::uint32_t>::max)()) {
				return false;
			}

			++cursor->freeCursorRefCount;
			a_state.CursorOwner = cursor;
			a_state.CursorOwned = true;
			return true;
		}

		[[nodiscard]] bool ReleaseCursor(OwnershipState& a_state)
		{
			if (!a_state.CursorOwned) {
				return true;
			}
			auto* cursor = RE::MenuCursor::GetSingleton();
			if (!cursor) {
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
			}
			a_state.CursorOwner = nullptr;
			a_state.CursorOwned = false;
			return true;
		}

		[[nodiscard]] bool SetPauseOwned(OwnershipState& a_state, bool a_owned)
		{
			if (a_state.PauseOwned == a_owned) {
				return true;
			}

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
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
			return true;
		}

		[[nodiscard]] bool SetBlurOwned(OwnershipState& a_state, bool a_owned)
		{
			if (a_state.BlurOwned == a_owned) {
				return true;
			}

			auto* blur = RE::UIBlurManager::GetSingleton();
			if (!blur) {
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
			return true;
		}

		[[nodiscard]] bool ReleaseCoreOwnership(OwnershipState& a_state)
		{
			if (!HasAnyCoreOwnership(a_state)) {
				PublishDisposition(InputDisposition::PassThrough);
				return true;
			}

			PublishDisposition(ReleaseDisposition(a_state));
			const bool cursorReleased = ReleaseCursor(a_state);
			const bool layerReleased = SetLayerDisabled(a_state, false);
			if (a_state.Faulted) {
				PublishDisposition(ReleaseDisposition(a_state));
				return false;
			}
			const bool released = cursorReleased && layerReleased &&
			                      !HasAnyCoreOwnership(a_state);
			PublishDisposition(ReleaseDisposition(a_state));
			return released;
		}

		[[nodiscard]] bool ReleaseAllOwnership(OwnershipState& a_state)
		{
			PublishDisposition(ReleaseDisposition(a_state));

			const bool blurReleased = SetBlurOwned(a_state, false);
			const bool pauseReleased = SetPauseOwned(a_state, false);
			const bool coreReleased = ReleaseCoreOwnership(a_state);

			if (a_state.Faulted) {
				return false;
			}
			if (!blurReleased || !pauseReleased || !coreReleased ||
				HasAnyActiveOwnership(a_state)) {
				PublishDisposition(ReleaseDisposition(a_state));
				return false;
			}

			PublishDisposition(InputDisposition::PassThrough);
			logger::info("Menu ownership released; retained input layer remains dormant");
			return true;
		}

		[[nodiscard]] bool AcquireCoreOwnership(OwnershipState& a_state)
		{
			PublishDisposition(InputDisposition::Suppress);

			if (!EnsureRetainedLayer(a_state) || !SetLayerDisabled(a_state, true)) {
				if (!a_state.Faulted) {
					PublishDisposition(ReleaseDisposition(a_state));
				}
				return false;
			}
			if (!AcquireCursor(a_state)) {
				static_cast<void>(SetLayerDisabled(a_state, false));
				if (a_state.Faulted) {
					return false;
				}
				PublishDisposition(ReleaseDisposition(a_state));
				return false;
			}

			logger::info(
				"Menu core ownership acquired (retained input layer {}, free-cursor references {})",
				a_state.InputLayer->GetLayerID(),
				a_state.CursorOwner->freeCursorRefCount);
			return true;
		}

		void RecoverFault(OwnershipState& a_state)
		{
			if (HasAnyActiveOwnership(a_state)) {
				static_cast<void>(ReleaseAllOwnership(a_state));
			}
			PublishDisposition(ReleaseDisposition(a_state));
		}
	}

	void Install()
	{
		if (lifecycleInstalled.test_and_set(std::memory_order_acq_rel)) {
			logger::warn("Menu input lifecycle is already installed");
			return;
		}

		PublishDisposition(InputDisposition::PassThrough);
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
		ValidateOwners(state);
		if (state.Faulted) {
			RecoverFault(state);
			return;
		}

		const bool windowOpen = WindowManager::IsAnyWindowOpen();
		const bool wantsInput = WindowManager::IsAnyBlockingWindowOpened();
		const bool wantsPause = a_context.PauseAllowed && WindowManager::ShouldPauseGame();
		const bool wantsBlur = WindowManager::ShouldBlurBackground();
		if (a_context.Availability == HostAvailability::Transient) {
			if (HasAnyActiveOwnership(state)) {
				if (!ReleaseAllOwnership(state)) {
					return;
				}
			}
			PublishDisposition(InputDisposition::PassThrough);
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
				PublishDisposition(InputDisposition::PassThrough);
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
					RecoverFault(state);
				}
				return;
			}
		}

		if (wantsInput) {
			if (!HasCoreOwnership(state) && !AcquireCoreOwnership(state)) {
				if (state.Faulted) {
					RecoverFault(state);
				}
				return;
			}
		} else if (!ReleaseCoreOwnership(state)) {
			return;
		}

		static_cast<void>(SetPauseOwned(state, wantsPause));
		if (!state.Faulted) {
			static_cast<void>(SetBlurOwned(state, wantsBlur));
		}
		if (state.Faulted) {
			RecoverFault(state);
			return;
		}

		PublishDisposition(
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

	InputDisposition GetInputDisposition() noexcept
	{
		return inputDisposition.load(std::memory_order_acquire);
	}
}
