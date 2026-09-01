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
- On a gamepad, double-press Start to open it; one press closes it.
- Press `Escape` while the panel is open to return to the game.

Menu style, bindings, toggle modes, pause, background blur, font rendering mode,
font face, variable font weight, logical font size, and manual UI scale are configurable through
`Options > Open Settings` and
`Data/SFSE/Plugins/SFSEMenuFramework.ini`.
The bundled `CLASSIC`, `MODERN`, `SKYRIMDEFAULT`, and `STARFIELD` themes live in
`Data/SFSE/Plugins/SFSEMenuFrameworkThemes`. Additional JSON themes using
the same schema appear in the selector after the next game restart.
The framework defaults to the `CLASSIC` theme and bundled Jost 400 Book face
at 48 logical px, rasterized with FreeType native hinting. Jost 500 Medium is
also bundled. The `STARFIELD` theme pairs with the bundled Space Grotesk
variable face, which supports live weights from 300 through 700; weight 500 is
the intended starting point. At 4K, 32 logical px and UI scale 1.5 are the
intended size and scale starting point.
ASCII-named direct-child `.ttf` and `.otf` files placed in
`Data/SFSE/Plugins/Fonts` appear in the selector after the next game restart.
Font rendering mode, face, variable weight, font size, and UI-scale previews
apply live; weight, size, and scale commit when their controls are released, and `Save`
persists the current preview.
If the selected font is missing or cannot build the framework tries the
bundled Jost faces and then ImGui's embedded font. Custom font files are
trusted local mod assets. A standard OpenType `wght` axis is detected
automatically; other variation axes, color fonts, CFF2, and otherwise
specialized OpenType features are not controlled by the framework.
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
The ImGui context and `ImGui::GetIO().Fonts` atlas address remain stable across
live font changes, but cached `ImFont*` values do not. Consumers must reacquire
font pointers inside every render callback.
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

Original SFSE Menu Framework code is licensed under
[GPL-3.0-only](COPYING) with the
[Modding Exception and GPL-3.0 Linking Exception](EXCEPTIONS).
SKSE Menu Framework-derived portions remain GPL-3.0-only as detailed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Dear ImGui remains
available under its [MIT license](extern/imgui/LICENSE.txt).

This project is a Starfield port of
[SKSE Menu Framework 3 by SkyrimThiago at commit `928e01a`](https://github.com/QTR-Modding/SKSE-Menu-Framework-3/tree/928e01ab459822a8d233ab99f0419ea1de23c775).
Its MCP shell, window and event APIs, settings presentation, theme schema and
assets, font discovery and fallback flow, and modal-menu behavior are directly
adapted under GPL-3.0-only.

The DirectX 12 renderer, pre-`kPostDataLoad` Raw Input bridge, stable
registration snapshots, live atlas transaction, and variable-font controls are
Starfield-specific. Exact source revisions, borrowed implementation boundaries,
licenses, exceptions, and bundled-asset notices are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
