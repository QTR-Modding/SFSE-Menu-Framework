# Source layout

The framework DLL owns ImGui. Client mods use the separate SFSE-MCP SDK.

## Where to look

- `plugin.cpp`: SFSE entry point, messages and startup order.
- `PCH.h`: shared precompiled header.
- `api/`: exports called by client mods.
- `runtime/`: page/window registration, menu paths, events and HUD callbacks.
- `config/`: settings, saved state, favorites and archived menus.
- `appearance/`: fonts, themes, stars, wallpapers and custom cursors.
- `appearance/fonts/`: font discovery, glyph ranges, atlas building and font stacks.
- `ui/`: the Mod Control Panel and Settings window.
- `input/`: game input capture, shortcut binding and gamepad navigation.
- `lifecycle/`: startup and control of game input, cursor, pause and blur.
- `platform/win32/`: game-window discovery and Windows input handling.
- `rendering/`: D3D12 resources and render hooks.

## Ownership

Keep related state with the code that creates and releases it:

- `D3D12Renderer.cpp` manages the ImGui context and frame resources together.
  Fonts, theme images and their descriptors stay alive until the GPU finishes
  using them. `D3D12Texture.cpp` handles their shared upload path.
- `StreamlineUIPrototype.cpp` draws into the game's UI texture for frame generation.
  `PresentOverlay.cpp` handles frames without that UI pass. `OverlayCompositor.cpp`
  shares their drawing code; each path owns its submission and resource lifetimes.
- `CommandListState.cpp` captures and restores game drawing state.
  `VtableHooks.cpp` keeps hook validation, installation and rollback together.
- `Win32Platform.cpp` owns the window subclass and shared input state.
  Keyboard, pointer and ImGui-backend work live in the neighboring files.
- `InputCapture.cpp` and `BindingCapture.cpp` keep keyboard event matching
  with their input-capture and rebinding state machines. `GamepadNavigation.cpp`
  owns controller navigation and cursor switching. Gamepad input comes from
  Starfield; separate Win32-backend XInput polling is disabled.
- `CallbackRegistry.h` shares registration and callback-lifetime handling.
  Each event, input or HUD manager decides when to dispatch its callbacks.
- `MenuOwnership.cpp` pairs taking game input, cursor, pause and blur control
  with releasing them.
- `FontManager.cpp` coordinates live font replacement. The files under
  `appearance/fonts/` handle discovery, selection, building and per-callback
  font-stack cleanup.
- `ThemeImage.cpp` decodes wallpapers and cursor images; `WallpaperDrawing.cpp`
  fits wallpapers to windows. `CursorDrawing.cpp` reads cursor metadata and draws
  the pointer. `CursorManager.cpp` owns discovery and the selected image independently
  of themes; `ui/CursorSettings.cpp` owns its controls. Opacity and UI-scale previews reuse the loaded images.

Some of these files are larger because splitting their shared state or resource
lifetime would make changes harder to follow. Internal headers should expose only
what another file needs; keep storage and implementation details with their owner.
