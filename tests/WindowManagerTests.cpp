#include "runtime/WindowManager.h"

#include "appearance/fonts/ConsumerFontScope.h"
#include "config/FrameworkSettings.h"
#include "input/BindingCapture.h"
#include "input/GamepadNavigation.h"
#include "platform/win32/Win32Platform.h"
#include "runtime/EventManager.h"
#include "ui/McpGamepad.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace
{
    using namespace SFSEMenuFramework;

    bool freezeTime = true;
    bool blurBackground = true;
    unsigned hostNotifications{};

    void Check(bool a_condition, const char* a_message)
    {
        if (!a_condition) {
            std::cerr << a_message << '\n';
            std::exit(1);
        }
    }

    void __stdcall Render() {}

    void CheckEffects(bool a_blocking, bool a_pause, bool a_blur)
    {
        Check(WindowManager::IsAnyBlockingWindowOpened() == a_blocking,
            "unexpected aggregate input blocking");
        Check(WindowManager::ShouldPauseGame() == a_pause,
            "unexpected aggregate pause request");
        Check(WindowManager::ShouldBlurBackground() == a_blur,
            "unexpected aggregate blur request");
    }

    void TestExternalWindows()
    {
        auto* first = WindowManager::AddExternalWindow(Render);
        auto* second = WindowManager::AddExternalWindow(Render);
        Check(first && second, "external registration must succeed");
        Check(!first->IsOpen && first->BlockUserInput,
            "legacy external window flags must retain their defaults");
        Check(!WindowManager::IsAnyWindowOpen(), "registered windows start closed");
        CheckEffects(false, false, false);

        first->IsOpen = true;
        Check(WindowManager::IsAnyWindowOpen(), "open external window must be observed");
        CheckEffects(true, false, false);
        const auto generation = WindowManager::GetBlockingWindowOpenGeneration();
        Check(generation != 0, "blocking window must keep its input generation");

        const auto notificationsBeforePause = hostNotifications;
        Check(WindowManager::SetWindowPauseGame(first, true), "external pause opt-in must succeed");
        Check(hostNotifications > notificationsBeforePause, "pause changes must notify the host");
        freezeTime = false;
        CheckEffects(true, true, false);
        freezeTime = true;
        CheckEffects(true, true, false);
        Check(WindowManager::GetBlockingWindowOpenGeneration() == generation,
            "pause changes must not renew the blocking input generation");
        first->BlockUserInput = false;
        CheckEffects(false, true, false);
        first->BlockUserInput = true;
        Check(WindowManager::SetWindowPauseGame(second, true), "second window may request pause");
        second->IsOpen = true;
        Check(WindowManager::SetWindowPauseGame(first, false), "first window may stop pausing");
        CheckEffects(true, true, false);
        second->IsOpen = false;
        CheckEffects(true, false, false);
        Check(WindowManager::SetWindowPauseGame(second, false), "closed window may clear pause");

        const auto blurGeneration = WindowManager::GetBlockingWindowOpenGeneration();

        const auto notificationsBeforeOptIn = hostNotifications;
        Check(WindowManager::SetWindowBlurBackground(first, true),
            "registered external window must accept blur opt-in");
        Check(hostNotifications > notificationsBeforeOptIn,
            "live blur opt-in must notify the host");
        CheckEffects(true, false, true);
        Check(WindowManager::GetBlockingWindowOpenGeneration() == blurGeneration,
            "changing blur must not renew the blocking input generation");

        blurBackground = false;
        CheckEffects(true, false, false);
        blurBackground = true;
        CheckEffects(true, false, true);

        const auto notificationsBeforeOptOut = hostNotifications;
        Check(WindowManager::SetWindowBlurBackground(first, false),
            "registered external window must accept blur opt-out");
        Check(hostNotifications > notificationsBeforeOptOut,
            "live blur opt-out must notify the host");
        CheckEffects(true, false, false);

        first->IsOpen = false;
        Check(WindowManager::SetWindowBlurBackground(first, true),
            "closed external window may retain an explicit blur preference");
        CheckEffects(false, false, false);
        first->IsOpen = true;
        CheckEffects(true, false, true);
        first->IsOpen = false;
        CheckEffects(false, false, false);
        first->IsOpen = true;
        CheckEffects(true, false, true);

        second->IsOpen = true;
        CheckEffects(true, false, true);
        first->IsOpen = false;
        CheckEffects(true, false, false);
        Check(WindowManager::SetWindowBlurBackground(second, true),
            "a second external window may independently opt in");
        CheckEffects(true, false, true);
        first->IsOpen = true;
        Check(WindowManager::SetWindowBlurBackground(second, false),
            "one external window may opt out while another requests blur");
        CheckEffects(true, false, true);
        Check(WindowManager::SetWindowBlurBackground(first, false),
            "disabling the last open blur requester must succeed");
        CheckEffects(true, false, false);

        freezeTime = false;
        CheckEffects(true, false, false);
        Check(WindowManager::SetWindowBlurBackground(first, true),
            "blur opt-in must be independent of the freeze-time preference");
        CheckEffects(true, false, true);
        first->BlockUserInput = false;
        second->IsOpen = false;
        CheckEffects(false, false, true);
        Check(WindowManager::IsAnyWindowOpen(), "nonblocking windows remain open");
        WindowManager::CloseAllBlockingWindows();
        Check(first->IsOpen, "closing blocking windows must retain a nonblocking requester");
        CheckEffects(false, false, true);
        first->IsOpen = false;
        CheckEffects(false, false, false);
        freezeTime = true;
    }

    void TestBuiltInWindowsAndValidation()
    {
        auto* main = WindowManager::AddWindow(Render);
        auto* settings = WindowManager::AddWindow(Render);
        auto* external = WindowManager::AddExternalWindow(Render);
        Check(main && settings && external, "mixed window registration must succeed");
        Check(WindowManager::SetMainWindow(main), "main window registration must succeed");
        Check(!WindowManager::SetWindowPauseGame(nullptr, true), "null pause target must be rejected");
        Check(!WindowManager::SetWindowPauseGame(main, true), "built-in pause target must be rejected");
        Check(!WindowManager::SetWindowBlurBackground(nullptr, true),
            "null window must be rejected");
        Model::WindowInterface unknown;
        Check(!WindowManager::SetWindowPauseGame(&unknown, true), "unknown pause target must be rejected");
        Check(!WindowManager::SetWindowBlurBackground(&unknown, true),
            "unregistered window must be rejected");
        Check(!WindowManager::SetWindowBlurBackground(
            reinterpret_cast<Model::WindowInterface*>(std::uintptr_t{ 1 }), true),
            "invalid pointer must be rejected without being dereferenced");
        Check(!WindowManager::SetWindowBlurBackground(main, false),
            "client setter must reject the built-in main window");
        Check(!WindowManager::SetWindowBlurBackground(settings, false),
            "client setter must reject other built-in windows");

        Check(WindowManager::SetMainWindowOpen(true), "main window must open");
        CheckEffects(true, true, true);
        main->BlockUserInput = false;
        CheckEffects(false, false, false);
        Check(WindowManager::SetMainWindowOpen(false), "main window must close");

        settings->IsOpen = true;
        CheckEffects(true, true, true);
        settings->PauseGame = false;
        CheckEffects(true, false, true);
        settings->BlockUserInput = false;
        CheckEffects(false, false, false);
        settings->IsOpen = false;
        settings->BlockUserInput = true;
        settings->PauseGame = true;

        external->IsOpen = true;
        CheckEffects(true, false, false);
        Check(WindowManager::SetWindowBlurBackground(external, true),
            "external opt-in must work after main registration");
        CheckEffects(true, false, true);
        main->BlurBackground = false;
        blurBackground = false;
        settings->IsOpen = true;
        CheckEffects(true, true, false);
        main->IsOpen = true;
        CheckEffects(true, true, false);
        main->BlurBackground = true;
        blurBackground = true;
        CheckEffects(true, true, true);
        WindowManager::CloseAllBlockingWindows();
        Check(!main->IsOpen && !settings->IsOpen && !external->IsOpen,
            "closing blocking windows must close every blocking window");
        CheckEffects(false, false, false);
    }
}

// Only engine/UI side effects are stubbed; registration and aggregation are
// compiled from the production WindowManager.cpp translation unit.
namespace SFSEMenuFramework::FrameworkSettings
{
    bool GetFreezeTimeOnMenu() noexcept { return freezeTime; }
    bool GetBlurBackgroundOnMenu() noexcept { return blurBackground; }
}

namespace SFSEMenuFramework::Win32Platform
{
    bool PostHostWindowCallback() noexcept
    {
        ++hostNotifications;
        return true;
    }
}

namespace SFSEMenuFramework::BindingCapture
{
    void Acknowledge() noexcept {}
    void Abort() noexcept {}
}

namespace SFSEMenuFramework::ConsumerFontScope
{
    CallbackScope::CallbackScope(const char*) noexcept {}
    CallbackScope::~CallbackScope() noexcept = default;
}

namespace SFSEMenuFramework::GamepadNavigation
{
    bool ShouldDrawMouseCursor(std::uint64_t) noexcept { return true; }
    RepeatTiming ApplyRepeatTiming() noexcept { return {}; }
    void RestoreRepeatTiming(RepeatTiming) noexcept {}
    CloseTargetSnapshot SnapshotCloseTarget(std::uint64_t) noexcept { return {}; }
    bool ConsumeCloseRequestForNewlyRenderedTarget(
        std::uint64_t, CloseTargetSnapshot) noexcept { return false; }
}

namespace SFSEMenuFramework::McpGamepad
{
    void BeginWindows(bool) {}
    void EndWindows() {}
}

bool SFSEMenuFramework::EventManager::SetMainWindowState(
    std::atomic<bool>& a_state, bool a_open, bool) noexcept
{
    return a_state.exchange(a_open) != a_open;
}

int main()
{
    using SFSEMenuFramework::Model::WindowInterface;
    static_assert(std::is_standard_layout_v<WindowInterface>);
    static_assert(sizeof(WindowInterface) == 2 * sizeof(std::atomic<bool>));
    static_assert(offsetof(WindowInterface, IsOpen) == 0);
    static_assert(offsetof(WindowInterface, BlockUserInput) == sizeof(std::atomic<bool>));

    TestExternalWindows();
    TestBuiltInWindowsAndValidation();
    std::cout << "WindowManager tests passed\n";
    return 0;
}
