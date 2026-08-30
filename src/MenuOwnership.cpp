#include "MenuOwnership.h"

#include "D3D12Renderer.h"
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
			RE::BSInputEnableLayer* InputLayer{ nullptr };
			bool                    CursorOwned{ false };
			bool                    PauseOwned{ false };
			bool                    MissingDependencyWarned{ false };
			bool                    AllocationFailureWarned{ false };
			bool                    CursorOverflowWarned{ false };
			bool                    ReleaseCursorWarned{ false };
			bool                    ReleasePauseWarned{ false };
		};

		std::atomic<bool> updatePending{ false };
		std::atomic_flag  wrongThreadLogged{};
		std::atomic_flag  schedulerInstalled{};

		void Update();

		class MainThreadUpdateTask final : public RE::BSService::QueuedDelegate
		{
		public:
			void Run() override
			{
				if (Win32Platform::IsCurrentThreadHostWindowThread()) {
					Update();
				} else if (!wrongThreadLogged.test_and_set(std::memory_order_relaxed)) {
					logger::critical(
						"Native lifecycle task rejected: the BSService callback does not own the Starfield window");
				}

				updatePending.store(false, std::memory_order_release);
			}
		};

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

		[[nodiscard]] bool HasAnyOwnership(const OwnershipState& a_state) noexcept
		{
			return a_state.InputLayer || a_state.CursorOwned || a_state.PauseOwned;
		}

		[[nodiscard]] bool HasCompleteOwnership(const OwnershipState& a_state) noexcept
		{
			return a_state.InputLayer && a_state.CursorOwned && a_state.PauseOwned;
		}

		[[nodiscard]] bool WantsOwnership(bool a_platformReady) noexcept
		{
			const auto* mainWindow = WindowManager::GetMainWindow();
			if (!a_platformReady || !mainWindow ||
				!mainWindow->IsOpen.load(std::memory_order_acquire) ||
				!mainWindow->BlockUserInput.load(std::memory_order_acquire)) {
				return false;
			}

			const auto generation = WindowManager::GetMainWindowOpenGeneration();
			return D3D12Renderer::HasRecentMainWindowFrame(generation) &&
			       WindowManager::IsMainWindowOpenGeneration(generation) &&
			       mainWindow->IsOpen.load(std::memory_order_acquire) &&
			       mainWindow->BlockUserInput.load(std::memory_order_acquire);
		}

		void Acquire(OwnershipState& a_state)
		{
			auto* inputManager = RE::BSInputEnableManager::GetSingleton();
			auto* cursor = RE::MenuCursor::GetSingleton();
			auto* ui = RE::UI::GetSingleton();
			if (!inputManager || !cursor || !ui) {
				if (!a_state.MissingDependencyWarned) {
					a_state.MissingDependencyWarned = true;
					logger::warn(
						"Menu ownership deferred: input manager, menu cursor, or UI is not ready");
				}
				return;
			}

			if (cursor->freeCursorRefCount == (std::numeric_limits<std::uint32_t>::max)()) {
				if (!a_state.CursorOverflowWarned) {
					a_state.CursorOverflowWarned = true;
					logger::error("Menu ownership rejected: the free-cursor reference count is full");
				}
				return;
			}

			RE::BSInputEnableLayer* inputLayer{};
			if (!inputManager->AllocateNewLayer(&inputLayer, "SFSE Menu Framework") || !inputLayer) {
				if (!a_state.AllocationFailureWarned) {
					a_state.AllocationFailureWarned = true;
					logger::error("Menu ownership deferred: failed to allocate an input-enable layer");
				}
				return;
			}

			inputLayer->EnableUserEvent(userEventsToDisable, false);
			inputLayer->EnableOtherEvent(otherEventsToDisable, false);
			++cursor->freeCursorRefCount;
			ui->ModifyMenuPauseCounter(PauseSourceName(), true);

			a_state.InputLayer = inputLayer;
			a_state.CursorOwned = true;
			a_state.PauseOwned = true;
			logger::info(
				"Menu ownership acquired (input layer {}, free-cursor references {})",
				inputLayer->GetLayerID(),
				cursor->freeCursorRefCount);
		}

		void Release(OwnershipState& a_state)
		{
			if (a_state.PauseOwned) {
				if (auto* ui = RE::UI::GetSingleton()) {
					ui->ModifyMenuPauseCounter(PauseSourceName(), false);
					a_state.PauseOwned = false;
				} else if (!a_state.ReleasePauseWarned) {
					a_state.ReleasePauseWarned = true;
					logger::warn("Menu pause release deferred: UI is unavailable");
				}
			}

			if (a_state.CursorOwned) {
				if (auto* cursor = RE::MenuCursor::GetSingleton()) {
					if (cursor->freeCursorRefCount > 0) {
						--cursor->freeCursorRefCount;
					} else if (!a_state.ReleaseCursorWarned) {
						a_state.ReleaseCursorWarned = true;
						logger::warn(
							"The free-cursor reference count was already zero during release");
					}
					a_state.CursorOwned = false;
				} else if (!a_state.ReleaseCursorWarned) {
					a_state.ReleaseCursorWarned = true;
					logger::warn("Free-cursor release deferred: MenuCursor is unavailable");
				}
			}

			if (a_state.InputLayer) {
				a_state.InputLayer->EnableUserEvent(userEventsToDisable, true);
				a_state.InputLayer->EnableOtherEvent(otherEventsToDisable, true);
				a_state.InputLayer->DecRef();
				a_state.InputLayer = nullptr;
			}

			if (!HasAnyOwnership(a_state)) {
				logger::info("Menu ownership released");
			}
		}

		void Update()
		{
			auto& state = GetState();
			const bool platformReady = D3D12Renderer::UpdatePlatform();
			const bool desired = WantsOwnership(platformReady);

			if (desired) {
				if (HasCompleteOwnership(state)) {
					D3D12Renderer::SetPlatformInputEnabled(true);
					return;
				}
				if (HasAnyOwnership(state)) {
					D3D12Renderer::SetPlatformInputEnabled(false);
					Release(state);
					return;
				}
				Acquire(state);
				D3D12Renderer::SetPlatformInputEnabled(HasCompleteOwnership(state));
				return;
			}

			D3D12Renderer::SetPlatformInputEnabled(false);
			if (HasAnyOwnership(state)) {
				Release(state);
			}
		}

		void ScheduleUpdate()
		{
			if (updatePending.exchange(true, std::memory_order_acq_rel)) {
				return;
			}

			if (Win32Platform::IsCurrentThreadHostWindowThread()) {
				Update();
				updatePending.store(false, std::memory_order_release);
				return;
			}

			auto* queue = RE::BSService::TaskQueue::GetSingleton();
			if (!queue) {
				updatePending.store(false, std::memory_order_release);
				return;
			}

			// The ownership-transfer check is adapted from OSF UI's
			// NativeMainThreadQueue at commit
			// 14b7565bbc7689b07fdccdb74525b9505f9f0dd6. OSF UI is GPL-3.0
			// with Modding and GPL-3.0 Linking Exceptions. QueueTask nulls the
			// pointer only when the engine accepts the delegate; otherwise this
			// tick is dropped and retried by the next permanent-task invocation.
			RE::BSService::QueuedDelegate* task = new MainThreadUpdateTask();
			queue->QueueTask(task);
			if (task) {
				delete task;
				updatePending.store(false, std::memory_order_release);
			}
		}
	}

	void Install(const SFSE::TaskInterface& a_taskInterface)
	{
		if (schedulerInstalled.test_and_set(std::memory_order_acq_rel)) {
			logger::warn("Menu input lifecycle scheduler is already installed");
			return;
		}

		a_taskInterface.AddPermanentTask([] {
			ScheduleUpdate();
		});
		logger::info("Menu input lifecycle scheduler installed");
	}
}
