# Source layout

The framework DLL owns ImGui. Client mods use the separate SFSE-MCP SDK.

## Where to look

- `plugin.cpp`: SFSE entry point, messages and startup order.
- `PCH.h`: shared precompiled header.
- `api/`: exports called by client mods.
- `runtime/`: page/window registration, menu paths, events and HUD callbacks.
- `config/`: settings, saved state, favorites and archived menus.
- `appearance/`: fonts, themes, stars and wallpapers.
- `appearance/fonts/`: font discovery, glyph ranges, atlas building and font stacks.
- `ui/`: the Mod Control Panel and Settings window.
- `input/`: game input capture, shortcut binding and gamepad navigation.
- `lifecycle/`: startup and control of game input, cursor, pause and blur.
- `platform/win32/`: game-window discovery and Windows input handling.
- `rendering/`: D3D12 resources and render hooks.

## Ownership

Keep related state with the code that creates and releases it:

- `D3D12Renderer.cpp` manages the ImGui context and frame resources together.
  Fonts, wallpapers and their descriptors stay alive until the GPU finishes
  using them. `D3D12Texture.cpp` handles their shared upload path.
- `RenderHooks.cpp` installs the Scaleform hooks.
  `D3D12CommandListHooks.cpp` handles command-list hooks and device checks.
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
- `WallpaperImage.cpp` decodes images; `WallpaperDrawing.cpp` fits them to
  windows. Opacity previews reuse the loaded image.

Some of these files are larger because splitting their shared state or resource
lifetime would make changes harder to follow. Internal headers should expose only
what another file needs; keep storage and implementation details with their owner.
