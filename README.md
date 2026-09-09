# SFSE Menu Framework

SFSE Menu Framework is a native Starfield Script Extender plugin that lets
other C++ SFSE plugins add ImGui pages and windows without owning a renderer,
input hook, ImGui context, or ImGui implementation.

## Requirements

- Starfield 1.16.244
- SFSE 0.2.21
- Address Library for Starfield 1.16.244

Building also requires Xmake 3.0.9 or newer and a C++23-capable MSVC toolchain.

## Controls and appearance

- Press `F1` once to open or close the Mod Control Panel.
- On a gamepad, double-press Start to open it; one press closes it.
- Use the D-pad or left stick to navigate and `A` to activate. `B` cancels the
  current interaction or selection first; with nothing selected, it closes the
  focused built-in window.
- Hold `X`, then use the left stick to move a window or the D-pad to resize it.
- Controller input hides the software cursor; moving or clicking the mouse restores it.
- Press `Escape` while it is open to return control to Starfield.

`Options > Open Settings` controls the theme, input bindings, toggle modes,
pause, blur and background opacity, font face, variable weight, logical size,
UI scale, rendering mode, and optional language glyph ranges. Appearance changes
preview live and `Save` persists them.

The shortcut controls follow the active input device. Select the displayed
binding, then press and release the replacement key or controller button.
`Escape` cancels capture. In shortcut warnings, `Escape` or controller `B`
cancels the change; `Clear` disables that device's binding after confirmation.

The default appearance uses the `STARFIELD` theme, Space Grotesk variable font
at weight 300, 40 logical pixels, 100% UI scale, and FreeType auto-hinting. The
optional `CONSTELLATION` theme adds procedural stars; `BLACKEST SEA` pairs white
stars with a SkyrimDefault-inspired black-and-white interface. `THE VOID` is
the same monochrome preset without stars. All three use the
live background-opacity control. `UNITY` adds a cosmic wallpaper and gold
accents. Custom themes can supply
their own PNG/JPEG wallpapers. Wallpaper opacity and dimming preview live and
save when the slider is released. Jost Book and
Medium are bundled as fallbacks. Additional direct-child `.ttf`
and `.otf` files in `Data/SFSE/Plugins/Fonts` become available after restart.
Additional theme JSON files using the bundled schema belong in
`Data/SFSE/Plugins/SFSEMenuFrameworkThemes`.

Slash-delimited registrations form a collapsible navigation tree. Top-level
sections can be searched, favorited, archived, and restored. Menu state and
window placement are stored under `Data/SFSE/Plugins`. Main and Settings window
layouts survive restarts in display-relative coordinates; `Options > Reset
Windows` restores both to their defaults.

Prefix a literal slash in a menu name with a backslash (`\/`). The separate
SDK provides `FullPathAddSectionItem`, `RenameSection`, `DeleteSection`, and
`GetMenuFrameworkAPIVersion`. Use API version `1` to detect runtime
rename/delete support. `GetMenuFrameworkVersion` remains a legacy
source-compatible release projection and must not be used as a capability gate.

The panel can open as soon as Starfield's window and renderer are ready, before
`kPostDataLoad`. Consumer callbacks that use game data must gate that work at
their own appropriate SFSE lifecycle boundary.

## Creating themes

Create a uniquely named `.json` file directly in
`Data/SFSE/Plugins/SFSEMenuFrameworkThemes`. Its filename without the extension
becomes the uppercase name in Settings. Use a printable ASCII name of at most
63 characters. Restart to discover new files; reselecting an existing theme
reloads its JSON and image.

Use a bundled theme as a starting point. Fields are optional and inherit the
built-in dark style. `ImGuiCol` contains named colors in `#RRGGBBAA` format.
Top-level ImGui style fields control padding, spacing, borders, rounding and
alignment; two-dimensional values use arrays such as `"WindowPadding": [20, 16]`.
`Alpha` fades everything, including text. Use background color alpha and the
background-opacity setting when text should stay fully visible.

For a wallpaper, package a PNG or JPEG alongside the JSON, for example:

```json
{
  "Backdrop": {
    "Type": "Wallpaper",
    "Image": "wallpapers/my-theme.png",
    "Opacity": 1.0,
    "Darkening": 0.0
  },
  "ImGuiCol": {
    "Text": "#F5F1E8FF",
    "WindowBg": "#050710EF",
    "ChildBg": "#00000000"
  }
}
```

Image paths are relative to the JSON's directory and must stay inside it.
Use printable ASCII paths, PNG/JPG/JPEG files up to 32 MiB, and dimensions up to
4096 pixels per side. The image fills the main and Settings windows using a
centered crop, preserving its proportions. Transparent child backgrounds reveal
the same continuous wallpaper beneath the main panel's sidebar and content.
The image stays fixed when content scrolls. Client-created standalone windows
inherit the style colors but do not automatically receive the backdrop.

`Opacity` and `Darkening` range from 0 to 1 and default to 1 and 0.
User wallpaper opacity multiplies the theme's opacity; wallpaper dimming
adds further darkening. Background opacity also fades the wallpaper. None of
these controls dims text or reloads the image. Unreadable images reject the
theme selection; a failed GPU upload shows the panel colors and an error in
Settings.

For procedural stars use `"Type": "Stars"`, with optional `StarColor`,
`AccentColor` (`#RRGGBBAA`), and `Density` (0–1). Omit `Backdrop` or use
`"Type": "None"` for plain panels.

Distribute the JSON and artwork in those same folders as a normal mod.
No C++ or custom DLL is needed. Fonts remain a separate user selection.
Include the required licenses for any fonts or artwork you redistribute.

## C++ client API

Clients use the separate MIT-licensed, header-only
[SFSE-MCP](https://github.com/QTR-Modding/SFSE-MCP) package:

```cpp
#include <SFSEMCP/SFSEMenuFramework.hpp>

void __stdcall DrawSettings()
{
    ImGuiMCP::TextUnformatted("Hello from Starfield");
    if (ImGuiMCP::Button("Increment")) {
        // Handle the button.
    }
}

void RegisterMenu()
{
    SFSEMenuFramework::SetSection("My Plugin");
    SFSEMenuFramework::AddSectionItem("Settings/General", &DrawSettings);
}
```

Register during SFSE `kPostLoad`. The framework exports
`SFSEPlugin_Preload` so its DLL and symbols are mapped before ordinary client
plugins load.

The client compiles and links no Dear ImGui implementation. Every `ImGuiMCP`
wrapper resolves an `ig*` export from `SFSEMenuFramework.dll`, and the
framework executes the call against its own ImGui context.

### Standalone windows

```cpp
MENU_WINDOW window{};

void __stdcall DrawWindow()
{
    bool open = window->IsOpen.load();
    if (ImGuiMCP::Begin("My Plugin Window", &open)) {
        ImGuiMCP::TextUnformatted("Standalone content");
    }
    ImGuiMCP::End();
    window->IsOpen.store(open);
}

void RegisterWindow()
{
    window = SFSEMenuFramework::AddWindow(&DrawWindow, true);
}
```

`WindowInterface::IsOpen` and `BlockUserInput` are atomic. The second
`AddWindow` parameter is named `blockUserInput` because it initializes
`BlockUserInput`; correcting the old parameter name does not affect C++ call
compatibility. The `AddWindowWithView` signature is retained; Starfield creates a
normal framework window and ignores `viewName` because the pinned Skyrim host
never implemented the view-specific export.

### Lifecycle, input, and HUD callbacks

`AddEvent` exposes `kOpenMenu`, `kCloseMenu`, `kBeforeRender`, and
`kAfterRender`. Higher priorities run first. `AddInputEvent` can consume a
native `RE::InputEvent` by returning `true`. `AddHudElement` renders every
framework frame before windows. Delete the returned registration object to
unregister it.

HUD, page, and window callbacks run with the framework's ImGui context active.
Input callbacks do not run on the render path and must not call ImGui. No
callback may let an exception cross the framework boundary.

Lifecycle event callbacks run on the render thread, but outside an active ImGui
frame: `kBeforeRender` is dispatched before `ImGui::NewFrame()` and
`kAfterRender` after `ImGui::Render()`. They must not call `ImGuiMCP`.

`LoadTexture` and `DisposeTexture` remain in SFSE-MCP for source
compatibility, but texture support is intentionally deferred. This host does
not currently export them, so loading returns a null texture and disposal is a
no-op.

### Fonts and icons

`SFSEMenuFramework::PushFont` selects a discovered font by filename or stem.
`FontAwesome::PushSolid`, `PushRegular`, and `PushBrands` select the bundled
icon faces; pair a successful push with `FontAwesome::Pop`. Cached raw
`ImFont*` pointers are invalidated by live font rebuilds, so clients should use
the named font helpers inside each render callback.

## Build

```powershell
git clone https://github.com/QTR-Modding/SFSE-MCP.git
git -C SFSE-MCP checkout ed331dab06b3055d2d6731a471bccd3587048a17
git clone --recurse-submodules https://github.com/QTR-Modding/SFSE-Menu-Framework.git
cd SFSE-Menu-Framework
xmake f -m releasedbg
xmake
xmake package
powershell -File scripts/Verify-Package.ps1 `
    -Archive build/packages/SFSEMenuFramework-0.15.0.zip `
    -BuiltDll build/windows/x64/releasedbg/SFSEMenuFramework.dll
```

Every framework build is signed and checked against the adjacent SFSE-MCP
checkout's public-key pin. Configure `SFSE_MF_SIGNING_CERT` with the exact
CurrentUser/My signing certificate thumbprint; builds fail if the key is missing
or does not match. The private key is never part of the repository or package.

`scripts/signing/Initialize-Signing.ps1` creates a non-exportable signing key once.
Keep that key: creating another one makes existing clients reject future builds.
Forks may use their own key and rebuild the SDK with its corresponding public pin.

To generate a Visual Studio solution:

```powershell
xmake project -k vsxmake
```

## License

Original SFSE Menu Framework code is licensed under
[GPL-3.0-only](COPYING) with the
[Modding Exception and GPL-3.0 Linking Exception](EXCEPTIONS).
SKSE Menu Framework-derived portions remain GPL-3.0-only as detailed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This project is a Starfield port of selected behavior from
[SKSE Menu Framework 3 by SkyrimThiago through accepted base `8a366c4`](https://github.com/QTR-Modding/SKSE-Menu-Framework-3/tree/8a366c4a3db7317655cec8379e48c43140c9fe7d),
plus the user-authored press-to-bind changes from `5b269fa` through
[`e297bbd`](https://github.com/QTR-Modding/SKSE-Menu-Framework-3/tree/e297bbdadd1d54c862068f714672db4eae4f93ff).
The DirectX 12 renderer, early Raw Input bridge, stable callback snapshots,
live font-atlas transaction, and variable-font controls are Starfield-specific.
Exact source revisions, borrowed implementation boundaries, licenses,
exceptions, and bundled-asset notices are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
