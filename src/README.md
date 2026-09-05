# Source architecture

This directory contains the private implementation of SFSE Menu Framework.
Client mods use the separate header-only SFSE-MCP package. The framework DLL
owns ImGui and exposes the matching direct and generated cimgui exports.

The implementation is organized by responsibility:

- `api`: direct SFSE-MCP exports and host-internal callback types.
- `runtime`: lifecycle/HUD dispatch, panel/window registration, callback snapshots, and consumer validation.
- `config`: framework settings, persistence, and root-menu visibility.
- `appearance`: live font/theme ownership; `appearance/fonts` separates font
  discovery, glyph ranges, atlas construction, and consumer stack isolation.
- `ui`: the framework control panel and settings window.
- `input`: the game-input capture hook, consumer input callbacks, and keyboard suppression handoff.
- `lifecycle`: framework startup and ownership of game input, cursor, pause, and blur.
- `platform/win32`: host-window discovery, subclassing, and keyboard/pointer routing.
- `rendering`: D3D12/ImGui lifetime and render-hook installation.

`plugin.cpp` owns only the SFSE entry point, message listener, and top-level
installation order. `PCH.h` is the target-wide precompiled header.

## Cohesive larger units

Some files remain larger when one shared lock or lifetime makes a split harder to audit:

- `rendering/D3D12Renderer.cpp` owns the ImGui context, D3D12 backend,
  font-atlas resource retirement, descriptor-heap restoration, and frame
  lifecycle as one GPU transaction.
- `input/InputCapture.cpp` owns one input-device hook and its lossless keyboard-edge token protocol.
- `runtime/CallbackRegistry.h` shares callback lifetime and snapshot handling
  across lifecycle events, HUD callbacks, and input callbacks. Each manager
  retains its own dispatch policy.
- `lifecycle/MenuOwnership.cpp` owns the balanced acquisition and release of game input, cursor, pause, and blur.

Within `platform/win32`, `Win32Platform.cpp` owns host-window discovery,
subclass lifetime, and raw-packet reading/type dispatch; `Win32Keyboard.cpp`
owns toggle-key state,
`Win32Pointer.cpp` owns cursor and capture state, and `Win32Backend.cpp`
owns the queued-input and Dear ImGui backend transactions. Their few shared
atomics live in one `SharedState` object owned by `Win32Platform.cpp` and are
visible only through `Win32PlatformInternal.h`.

Within `rendering`, `RenderHooks.cpp` owns the Scaleform render-pass seam and
transactional vtable patching, while `D3D12CommandListHooks.cpp` owns the
command-list hooks, Streamline/native-device validation, self-test, and render
region tracking.

Within `appearance/fonts`, `FontCatalog.cpp` validates and discovers files and
their sidecars, `GlyphRanges.cpp` builds optional Unicode coverage,
`FontBuildPlan.cpp` selects assets and fallback roles, and `FontComposer.cpp`
performs Dear ImGui composition and validation. `FontAtlasBuilder.cpp` owns the
explicit retry ladder, while `ConsumerFontScope.cpp` contains each plugin
callback's font-stack mutations. `appearance/FontManager.cpp` only coordinates
live generation replacement and GPU upload.

Internal headers expose only the contracts required across these units.
Registry storage and lifecycle state remain private to their owning translation
units; only the per-entry state required by another unit crosses a private
internal header.
