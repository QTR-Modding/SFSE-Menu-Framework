# SFSE Menu Framework

SFSE Menu Framework is a native Starfield Script Extender plugin for building ImGui menus directly in C++.

## Requirements

- Starfield 1.16.244
- [SFSE](https://sfse.silverlock.org/)
- Address Library for SFSE Plugins matching Starfield 1.16.244
- [Xmake](https://xmake.io/) 3.0.9 or newer
- A C++23-capable MSVC toolchain

## Controls

- Press `F1` once to open or close the Mod Control Panel.
- On a gamepad, double-press the left bumper to open it; one press closes it.
- Press `Escape` while the panel is open to return to the game.

Bindings, toggle modes, pause, and background blur are configurable through
`Options > Open Settings` and
`Data/SFSE/Plugins/SFSEMenuFramework.ini`.
The main window follows SKSE Menu Framework's shell: slash-delimited entries
form a collapsible navigation tree, the search box filters top-level mod
sections, and favorite sections sort before the remaining alphabetical list.
Sections can also be archived and restored through `Options`; that state is
stored in
`Data/SFSE/Plugins/SFSEMenuFrameworkMenuConfig.json`.
`Options > Resume Game` leaves the ImGui windows visible while returning
control to Starfield. Close and reopen the MCP to make it modal again.
Built-in window placement is retained across restarts in
`Data/SFSE/Plugins/SFSEMenuFramework.imgui.ini`; `Reset Windows` replaces that
saved placement with the centered defaults.
The panel can open during startup as soon as Starfield's window and renderer are
ready. As soon as the open panel has produced its first visible frame, its ImGui
software cursor, relative mouse movement, mouse buttons and wheel, keyboard,
gamepad, and bounded native-input routing are active even before SFSE
`kPostDataLoad`. Starfield cursor, control-layer, pause, and blur ownership
activate after `kPostDataLoad`.

## C++ consumer API

An SFSE plugin can register a page without installing its own renderer, window
hook, or input hook:

```cpp
#include <SFSEMenuFramework/SFSEMenuFramework.h>

void __stdcall RenderSettings() noexcept
{
    ImGui::TextUnformatted("Hello from my SFSE plugin");
}

void RegisterMenu()
{
    if (!SFSEMenuFramework::IsInstalled() ||
        !SFSEMenuFramework::SetSection("My Plugin")) {
        return;
    }

    SFSEMenuFramework::AddSectionItem("Settings/General", &RenderSettings);
}
```

The title passed to `AddSectionItem` may contain `/` separators. Combined
with the current section, it recreates SKSE Menu Framework's arbitrary-depth
menu path without changing the binary interface. Use `SetSection` for the
top-level menu name and place nested path separators in `AddSectionItem`.

Consumers can also register a separate, resizable ImGui window with the same
SKSE Menu Framework-style control surface:

```cpp
SFSEMenuFramework::Model::WindowInterface* window{};

void __stdcall RenderWindow() noexcept
{
    bool open = window->IsOpen.load();
    ImGui::Begin("My Plugin Window", &open);
    ImGui::TextUnformatted("Consumer-owned window");
    ImGui::End();
    window->IsOpen.store(open);
}

void RegisterWindow()
{
    window = SFSEMenuFramework::AddWindow(&RenderWindow, true);
    if (window) {
        window->IsOpen.store(true);
    }
}
```

`AddWindow` returns a stable process-lifetime `WindowInterface`, matching the
original framework's assignment model. `IsOpen` and `BlockUserInput` are atomic
and may be changed directly. Every open blocking window participates in the
same cursor, input, pause, and blur ownership; nonblocking windows continue to
render without taking Starfield input. `GetMainWindow`,
`IsAnyBlockingWindowOpened`, `SetHotkeyEnabled`, and `IsHotkeyEnabled` are also
available. The configured hotkey controls only the main Mod Control Panel;
`Escape` can still close it while hotkeys are disabled. Opening or re-blocking
a framework-rendered ImGui window can currently center the software cursor
once; subsequent mouse movement remains unrestricted.

Consumers can subscribe to the same lifecycle events exposed by SKSE Menu
Framework:

```cpp
SFSEMenuFramework::Model::Event* lifecycleEvent{};

void __stdcall OnFrameworkEvent(
    SFSEMenuFramework::Model::EventType type) noexcept
{
    // kOpenMenu, kCloseMenu, kBeforeRender, or kAfterRender
}

void RegisterEvents()
{
    lifecycleEvent = SFSEMenuFramework::AddEvent(&OnFrameworkEvent, 10.0F);
}
```

Higher priorities run first; equal priorities retain registration order.
Deleting the returned `Event` unregisters it. Deletion from another thread
waits for an executing callback to finish and prevents any later callback from
starting; self-deletion lets that current callback return normally.
`kOpenMenu` and `kCloseMenu` report framework-routed main Mod Control Panel
state edges only. Consumer-owned windows do not emit them, and directly storing
through `GetMainWindow()->IsOpen` bypasses lifecycle delivery. Consumers should
treat the main window's `IsOpen` as read-only and must not race direct stores
against the framework's controls. All four callbacks run on the render thread.
Queued open/close edges are drained before
the following framework frame's `kBeforeRender`. They are ordered edge history,
not state snapshots: if the MCP changes again before delivery, query
`GetMainWindow()->IsOpen` separately for its current state. `kBeforeRender`
and `kAfterRender` use the same listener snapshot around each successfully
recorded framework frame; a listener explicitly removed between them is skipped
for `kAfterRender`. Lifecycle callbacks run outside an active consumer ImGui
frame and must not issue ImGui commands.

Call panel and window registration from the SFSE `kPostLoad` message so it works
regardless of DLL load order. Consumer projects must compile the four Dear ImGui
core sources at version 1.90.8, commit
`6f7b5d0ee2fe9948ab871a530888a6dc5c960700`, and must not compile or initialize
an ImGui platform or renderer backend. The pinned `imconfig.h` must remain
unmodified. Registration validates the public and internal ImGui layouts and
rejects known non-default configuration families; the source-revision token is
the consumer's declaration that it compiled the pinned core sources. The SDK
header binds the consumer's ImGui copy to the framework context and allocator
for each callback. Render callbacks must be `noexcept` and must balance every
ImGui `Begin`/`End` and `Push`/`Pop` operation.
Because a registered page can render before `kPostDataLoad`, its callback must
gate any data-dependent engine access at the consumer plugin's own lifecycle
boundary.

## Build

Clone the repository with its submodules, then build the Release-with-debug-information configuration:

```powershell
git clone --recurse-submodules https://github.com/QTR-Modding/SFSE-Menu-Framework.git
cd SFSE-Menu-Framework
xmake f -m releasedbg
xmake
```

To generate a Visual Studio solution:

```powershell
xmake project -k vsxmake
```

## License

SFSE Menu Framework is licensed under [GPL-3.0-only](COPYING) with the [Modding Exception and GPL-3.0 Linking Exception](EXCEPTIONS). Dear ImGui remains available under its [MIT license](extern/imgui/LICENSE.txt).

This project is a Starfield port of [SKSE Menu Framework 3 by SkyrimThiago at commit `928e01a`](https://github.com/QTR-Modding/SKSE-Menu-Framework-3/tree/928e01ab459822a8d233ab99f0419ea1de23c775). Its early framework-registration and lazy-backend ordering, `AddWindow`/`WindowInterface`/`GetMainWindow` API, aggregate blocking-window behavior, hotkey enable control, software-cursor behavior, preserve-PrintScreen modal policy, toggle and close behavior, and fresh `LB` + double-press gamepad default are directly adapted under GPL-3.0.

The MCP shell is directly adapted from that revision's `include/UI.h`,
`src/UI.cpp`, `include/RootMenuConfig.h`, and
`src/RootMenuConfig.cpp`: slash-path navigation, recursive tree selection,
root-only text filtering, favorites-first ordering, archive confirmation and
recovery, the Options menu, the separate Settings-window presentation, and
the visible-but-nonmodal Resume Game behavior. The port rebuilds a
render-thread view from immutable `PanelRegistry` snapshots and stable panel
handles instead of copying the source's unsynchronized raw-pointer tree. Its
favorite/archive controls use ASCII labels because the SFSE font atlas does
not yet include SKSE Menu Framework's Font Awesome assets. English UI labels
are embedded rather than copied into translation sidecars. Unlike the pinned
source's process-only placement state, the Starfield port retains built-in
window placement through its ImGui ini file.

The lifecycle event enum, RAII listener API, priority-order intent, main-menu
open/close intent, and before/after-render boundaries are also directly adapted
from that pinned source. The pinned implementation does not store the supplied
priority and leaves its RAII handle uninitialized; this port deliberately
corrects both defects, validates listener handles, and uses quiescent,
snapshot-based render-thread dispatch. Its Open/Close delivery is deferred to a
safe Starfield render boundary instead of running synchronously inside the state
mutation.

The pinned SKSE Menu Framework source centers the cursor whenever a blocking
window opens; the current port retains that one-time behavior.
Starfield's pre-`kPostDataLoad` relative-mouse bridge is an independent Windows
Raw Input implementation; SKSE Menu Framework has no equivalent relative-motion
path. Its only physical cursor placement transfers the already-visible virtual
position into Starfield's normal cursor route at that lifecycle handoff.

The verified vtable-hook installation and guarded rollback pattern is adapted from [Toggle Dialogue Camera SF at commit `8021fa9`](https://github.com/QTR-Modding/ToggleDialogueCameraSF/tree/8021fa934591aac1c71266cc4abc5cb1c24e28d7), under GPL-3.0-or-later with its Modding and GPL-3.0 Linking Exceptions.

The Starfield cursor, control-layer, simulation-pause, and native-main-thread queue ownership-transfer protocols are adapted from [OSF UI at commit `14b7565`](https://github.com/ozooma10/osf-ui/tree/14b7565bbc7689b07fdccdb74525b9505f9f0dd6) by ozooma10, under GPL-3.0 with its Modding and GPL-3.0 Linking Exceptions.

Favorites and archive persistence uses
[JSON for Modern C++ 3.11.3](https://github.com/nlohmann/json/tree/v3.11.3),
which remains available under the MIT license included in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
