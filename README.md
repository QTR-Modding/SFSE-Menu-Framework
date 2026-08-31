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

Bindings, toggle modes, pause, and background blur are configurable in the
built-in Settings page and `Data/SFSE/Plugins/SFSEMenuFramework.ini`.
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

    SFSEMenuFramework::AddSectionItem("Settings", &RenderSettings);
}
```

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
`Escape` can still close it while hotkeys are disabled.

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

This project is a Starfield port of [SKSE Menu Framework 3 at commit `928e01a`](https://github.com/QTR-Modding/SKSE-Menu-Framework-3/tree/928e01ab459822a8d233ab99f0419ea1de23c775). Its early framework-registration and lazy-backend ordering, `AddWindow`/`WindowInterface`/`GetMainWindow` API, aggregate blocking-window behavior, hotkey enable control, software-cursor and cursor-centering behavior, preserve-PrintScreen modal policy, toggle and close behavior, and fresh `LB` + double-press gamepad default are directly adapted under GPL-3.0.

Starfield's pre-`kPostDataLoad` relative-mouse bridge is an independent Windows Raw Input implementation; SKSE Menu Framework has no equivalent relative-motion path.

The verified vtable-hook installation and guarded rollback pattern is adapted from [Toggle Dialogue Camera SF at commit `8021fa9`](https://github.com/QTR-Modding/ToggleDialogueCameraSF/tree/8021fa934591aac1c71266cc4abc5cb1c24e28d7), under GPL-3.0-or-later with its Modding and GPL-3.0 Linking Exceptions.

The Starfield cursor, control-layer, simulation-pause, and native-main-thread queue ownership-transfer protocols are adapted from [OSF UI at commit `14b7565`](https://github.com/ozooma10/osf-ui/tree/14b7565bbc7689b07fdccdb74525b9505f9f0dd6) by ozooma10, under GPL-3.0 with its Modding and GPL-3.0 Linking Exceptions.
