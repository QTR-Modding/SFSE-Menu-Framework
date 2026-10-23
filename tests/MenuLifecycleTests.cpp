// Exercise the production lifecycle against process-local engine and platform fakes.
// The plugin PCH is intentionally absent: no game relocations execute in this test.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace RE
{
    class BSFixedString
    {
    public:
        explicit BSFixedString(const char*) {}
    };

    class UI
    {
    public:
        static UI* GetSingleton();
        bool IsMenuOpen(const BSFixedString&) const;
    };

    class PlayerCharacter
    {
    public:
        static PlayerCharacter* GetSingleton();
    };
}

namespace SFSE
{
    class TaskInterface
    {
    public:
        void AddPermanentTask(void (*a_task)()) const;
    };
}

namespace logger
{
    template <class... Args>
    void critical(const char*, Args&&...) {}

    template <class... Args>
    void info(const char*, Args&&...) {}

    template <class... Args>
    void warn(const char*, Args&&...) {}
}

#include "../src/lifecycle/MenuLifecycle.cpp"

namespace
{
    using namespace SFSEMenuFramework;
    using InputDisposition = MenuOwnership::InputDisposition;

    struct Fixture
    {
        Win32Platform::HostWindowCallback Callback{};
        void (*PermanentTask)(){};
        bool PlayerPresent{};
        bool HostUsable{ true };
        bool Loading{};
        bool BlockingOpen{};
        bool RecentFrame{};
        bool CurrentGeneration{ true };
        bool Modal{};
        bool PlatformInput{};
        bool RenderEnabled{};
        bool CoreOwned{};
        bool FailRelease{};
        std::uint64_t Generation{};
        std::uint64_t RoutedGeneration{};
        int ReconcileCalls{};
        int ReleaseCalls{};
        InputDisposition Disposition{ InputDisposition::PassThrough };
    } fixture;

    void Check(bool a_value, const char* a_message)
    {
        if (!a_value) {
            std::fprintf(stderr, "Menu lifecycle test failed: %s\n", a_message);
            std::exit(1);
        }
    }

    void Reconcile()
    {
        Check(fixture.Callback != nullptr, "host-window callback registered");
        fixture.Callback();
    }

    void Open(std::uint64_t a_generation, bool a_visible = true)
    {
        fixture.BlockingOpen = true;
        fixture.Generation = a_generation;
        fixture.RecentFrame = a_visible;
        fixture.CurrentGeneration = true;
    }

    void Close()
    {
        fixture.BlockingOpen = false;
        fixture.Generation = 0;
    }

    void CheckFrameworkRoute(std::uint64_t a_generation, const char* a_message)
    {
        Check(fixture.Modal && fixture.PlatformInput &&
            fixture.RoutedGeneration == a_generation, a_message);
        Check(!fixture.CoreOwned, "framework-only routing acquires no gameplay ownership");
    }
}

RE::UI* RE::UI::GetSingleton()
{
    static UI ui;
    return &ui;
}

bool RE::UI::IsMenuOpen(const BSFixedString&) const
{
    return fixture.Loading;
}

RE::PlayerCharacter* RE::PlayerCharacter::GetSingleton()
{
    static PlayerCharacter player;
    return fixture.PlayerPresent ? &player : nullptr;
}

void SFSE::TaskInterface::AddPermanentTask(void (*a_task)()) const
{
    fixture.PermanentTask = a_task;
}

namespace SFSEMenuFramework::Translations
{
    bool Load(const std::filesystem::path&) { return true; }
}

namespace SFSEMenuFramework::FrameworkSettings
{
    bool Load() noexcept { return true; }
    std::filesystem::path BuildGamePath(std::wstring_view a_path) { return a_path; }
}

bool SFSEMenuFramework::McpWindow::Install() { return true; }

namespace SFSEMenuFramework::InputCapture
{
    bool Install() { return true; }
    void ArmFunctionalCapture() noexcept {}
    void SetModal(bool a_modal) noexcept { fixture.Modal = a_modal; }
    bool IsModal() noexcept { return fixture.Modal; }
}

namespace SFSEMenuFramework::Win32Platform
{
    InitializeResult Initialize() { return InitializeResult::Ready; }
    bool IsInitialized() noexcept { return true; }
    bool IsHostWindowUsable() noexcept { return fixture.HostUsable; }
    void SetHostWindowCallback(HostWindowCallback a_callback) noexcept
    {
        fixture.Callback = a_callback;
    }
    bool PostHostWindowCallback() noexcept { return true; }
}

namespace SFSEMenuFramework::D3D12Renderer
{
    bool SetPlatformInputEnabled(bool a_enabled, std::uint64_t a_generation)
    {
        fixture.PlatformInput = a_enabled;
        fixture.RoutedGeneration = a_generation;
        return true;
    }

    bool HasRecentBlockingWindowFrame(std::uint64_t a_generation) noexcept
    {
        return fixture.RecentFrame && a_generation == fixture.Generation;
    }
}

void SFSEMenuFramework::WindowManager::SetMainWindowRenderEnabled(bool a_enabled) noexcept
{
    fixture.RenderEnabled = a_enabled;
}

bool SFSEMenuFramework::WindowManager::IsAnyBlockingWindowOpened() noexcept
{
    return fixture.BlockingOpen;
}

std::uint64_t SFSEMenuFramework::WindowManager::GetBlockingWindowOpenGeneration() noexcept
{
    return fixture.Generation;
}

bool SFSEMenuFramework::WindowManager::IsBlockingWindowOpenGeneration(
    std::uint64_t a_generation) noexcept
{
    return fixture.CurrentGeneration && a_generation == fixture.Generation;
}

void SFSEMenuFramework::WindowManager::CloseAllBlockingWindows() noexcept { Close(); }

namespace SFSEMenuFramework::MenuOwnership
{
    void Install() {}

    void ReconcileOnHostWindowThread(const ReconcileContext& a_context)
    {
        Check(fixture.PlayerPresent, "gameplay ownership must never reconcile without a player");
        ++fixture.ReconcileCalls;
        fixture.CoreOwned = fixture.BlockingOpen &&
            a_context.Availability == HostAvailability::Interactive;
        fixture.Disposition = fixture.CoreOwned ?
            InputDisposition::RouteToMenu : InputDisposition::PassThrough;
    }

    void ReleaseOnHostWindowThread()
    {
        ++fixture.ReleaseCalls;
        if (fixture.CoreOwned && fixture.FailRelease) {
            fixture.Disposition = InputDisposition::Suppress;
            return;
        }
        fixture.CoreOwned = false;
        fixture.Disposition = InputDisposition::PassThrough;
    }

    InputDisposition GetInputDisposition() noexcept { return fixture.Disposition; }
}

int main()
{
    using namespace SFSEMenuFramework;

    const SFSE::TaskInterface tasks;
    Check(MenuLifecycle::InstallEarly(tasks), "early installation succeeds");
    Check(fixture.PermanentTask != nullptr, "host bootstrap task registered");

    // Initial main-menu input keeps the existing visible-generation requirement.
    Open(1, false);
    Reconcile();
    Check(!fixture.Modal && !fixture.PlatformInput,
        "unrendered startup menu cannot suppress native input");
    fixture.RecentFrame = true;
    Reconcile();
    CheckFrameworkRoute(1, "rendered startup menu routes through framework capture");
    Check(fixture.ReconcileCalls == 0 && fixture.ReleaseCalls == 0,
        "startup input does not touch engine ownership");
    Close();
    Reconcile();
    Check(!fixture.Modal && !fixture.PlatformInput, "closing startup menu restores input");

    Check(MenuLifecycle::ActivatePostDataLoad(), "post-data-load activation succeeds");
    Reconcile();
    Check(fixture.ReconcileCalls == 0 && fixture.ReleaseCalls == 1,
        "post-data-load alone does not imply that a player exists");

    fixture.PlayerPresent = true;
    Open(2);
    Reconcile();
    Check(fixture.CoreOwned && fixture.Modal && fixture.PlatformInput &&
        fixture.RoutedGeneration == 0, "live gameplay acquires normal menu ownership");
    Close();
    Reconcile();
    Check(!fixture.CoreOwned && !fixture.Modal && !fixture.PlatformInput,
        "closing the gameplay menu restores input");

    // Regression: load a save, return with the overlay closed, then open it.
    const auto gameplayReconciles = fixture.ReconcileCalls;
    fixture.PlayerPresent = false;
    Reconcile();
    Check(!fixture.Modal && !fixture.PlatformInput,
        "returning with a closed overlay keeps native input available");
    Open(3);
    Reconcile();
    CheckFrameworkRoute(3, "returned main menu remains interactive");
    Check(fixture.ReconcileCalls == gameplayReconciles,
        "returned main menu never enters gameplay ownership");
    fixture.Loading = true;
    Reconcile();
    Check(!fixture.RenderEnabled && !fixture.Modal && !fixture.PlatformInput,
        "loading after post-data-load suspends rendering and capture without a player");
    fixture.Loading = false;
    Reconcile();
    Check(fixture.RenderEnabled, "returned menu resumes rendering after loading finishes");
    CheckFrameworkRoute(3, "returned menu resumes framework capture after loading finishes");

    Close();
    Reconcile();
    Open(4, false);
    Reconcile();
    Check(!fixture.Modal && !fixture.PlatformInput,
        "returned main menu still waits for a rendered frame");
    fixture.RecentFrame = true;
    fixture.CurrentGeneration = false;
    Reconcile();
    Check(!fixture.Modal && !fixture.PlatformInput,
        "stale rendered generation cannot capture returned main-menu input");
    fixture.CurrentGeneration = true;
    Reconcile();
    CheckFrameworkRoute(4, "current returned main-menu generation can capture input");

    // A subsequent save reuses normal ownership, including an already-open overlay.
    fixture.PlayerPresent = true;
    Reconcile();
    Check(fixture.CoreOwned && fixture.Modal && fixture.PlatformInput &&
        fixture.RoutedGeneration == 0, "next save reacquires gameplay ownership");
    const auto beforeUnload = fixture.ReleaseCalls;
    fixture.PlayerPresent = false;
    Reconcile();
    Check(fixture.ReleaseCalls == beforeUnload + 1,
        "unloading with an open overlay releases previous gameplay ownership");
    CheckFrameworkRoute(4, "open overlay continues with framework-only input after unload");

    fixture.PlayerPresent = true;
    Reconcile();
    Check(fixture.CoreOwned, "another save can reacquire ownership after open-overlay unload");
    fixture.PlayerPresent = false;
    fixture.FailRelease = true;
    const auto beforeFailedRelease = fixture.ReleaseCalls;
    Reconcile();
    Check(fixture.CoreOwned && fixture.Modal && !fixture.PlatformInput,
        "failed release preserves native suppression and blocks platform routing");
    Close();
    Reconcile();
    Check(fixture.ReleaseCalls == beforeFailedRelease + 2,
        "pending release is retried on the next callback");
    Check(fixture.Modal && !fixture.PlatformInput,
        "closing overlay cannot clear suppression while its engine layer remains disabled");
    fixture.FailRelease = false;
    Reconcile();
    Check(!fixture.CoreOwned && !fixture.Modal && !fixture.PlatformInput,
        "successful release restores pass-through after the overlay closes");

    std::puts("Menu lifecycle regression tests passed");
}
