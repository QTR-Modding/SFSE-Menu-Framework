# SFSE Menu Framework

An in-game settings menu for C++ Starfield mods, based on SkyrimThiago's
SKSE Menu Framework. Mod authors use [SFSE-MCP](https://github.com/QTR-Modding/SFSE-MCP)
to add pages and windows; the framework handles ImGui, rendering and input.

## Requirements

- Starfield 1.16.244
- SFSE 0.2.21
- Address Library for Starfield 1.16.244

## Getting started

Install the mod with your mod manager, or copy the archive's `SFSE` folder
into Starfield's `Data` folder. Launch through SFSE, then press **F1**.

- **F1:** open or close the panel.
- **Gamepad Start:** double-press to open; press once to close.
- **D-pad / left stick:** navigate by direction within pages; Up/Down cycles the mod list.
  Select a slider before using Left/Right to adjust it.
- **A:** select a page or control. **B:** cancel an edit or popup, return from the
  page to the mod list, then close the panel.
- **Right stick:** scroll the current panel.
- **X:** toggle Options. **RB:** focus the selected page's controls.
- **Hold X:** ImGui's window move/resize controls.
- **Escape:** close the main panel.
- **Options > Resume Game:** return control to the game while leaving the panel visible.

Gamepad input hides the cursor; using the mouse brings it back.
Choose Xbox or PlayStation prompts in **Settings > Controls > Controller icons**.
Use Up/Down to select the favorite/archive buttons and A to activate them.
The menu can open before game data finishes loading.

## Settings

Open **Options > Open Settings** to change the theme, font, size, weight,
UI scale, opacity, shortcuts, pause or blur. Font changes preview live; press
**Save** to keep them. Background sliders save when you release them.
If saving a background change fails, Settings shows an error and reverts it.

To change a shortcut, select its binding and press and release the new key or
button. **Escape** cancels. **Clear** disables that device's shortcut after
confirmation; **Escape** or gamepad **B** dismisses a warning without changing it.

The default is **STARFIELD** with Space Grotesk at weight 300. Other themes are
**CONSTELLATION** (stars), **BLACKEST SEA** (black and white with stars),
**THE VOID** (black and white without stars), and **UNITY** (wallpaper and gold accents).

You can search, favorite, archive and restore mod sections. Settings and window
positions are saved under `Data/SFSE/Plugins`. Use **Options > Reset Windows**
to restore the main and Settings windows.

For more fonts, place `.ttf` or `.otf` files directly in
`Data/SFSE/Plugins/Fonts`, then restart the game.

## Menu sounds

In **Options > Open Settings > Sounds**, enable sounds and choose a file for
each event. Every event has its own toggle; volume and selections apply live
and save automatically. Sounds are off by default and independent of themes.

For custom sounds, put 16-bit PCM WAV files directly in
`Data/SFSE/Plugins/SFSEMenuFrameworkSounds`, then select **Reload sounds**.
Use mono/stereo audio, 8–192 kHz, up to 5 seconds and 4 MiB per file. Filenames
must be printable ASCII, at most 63 characters including `.wav`.
**Default** uses a quiet built-in tone; **Preview** plays your selection.
No changes to client mods are required for standard ImGui widgets.

## Adding a menu to your mod

Use the MIT-licensed [SFSE-MCP SDK](https://github.com/QTR-Modding/SFSE-MCP).
It is available as a single header or through vcpkg. You do not need to compile
ImGui into your plugin.

See the [example mod](https://github.com/QTR-Modding/SFSE-Menu-Framework-Example)
for working pages, windows, input listeners, events and HUD elements.
Register during SFSE `kPostLoad`; callbacks that need game data must wait until
that data is ready.

## Custom themes

Themes are JSON files in `Data/SFSE/Plugins/SFSEMenuFrameworkThemes`.
Copy a bundled theme to get started. No C++ or extra DLL is needed.

<details>
<summary>Theme format, stars and wallpapers</summary>

Use a unique printable ASCII filename of at most 63 characters, excluding
`.json`. The name appears in uppercase in Settings. Restart to discover a new
theme; reselect an existing one to reload its JSON and image.

Missing fields use the built-in dark style. `ImGuiCol` colors use
`#RRGGBBAA`; style values such as `"WindowPadding": [20, 16]` control layout.
`Alpha` fades everything, including text. Use background color alpha instead
when text should stay fully visible.

For a wallpaper:

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

- Image paths are relative to the JSON's folder and must stay inside it.
  Use printable ASCII paths, PNG/JPG/JPEG files up to 32 MiB, and images
  no larger than 4096 pixels per side.
- The image is centered and cropped to fill the main and Settings windows
  without stretching. Transparent child backgrounds reveal it beneath the
  sidebar and content. It stays fixed while scrolling.
- `Opacity` and `Darkening` range from 0 to 1; their defaults are 1 and 0.
  User wallpaper opacity multiplies the theme's opacity, and dimming adds
  darkness. Background opacity fades the wallpaper too. Text is unaffected.
- Unreadable images prevent the theme from loading. If the GPU upload fails,
  the panel keeps its colors and Settings shows an error; reselect to retry.
- Mod-created windows inherit the colors, but not the backdrop.

For stars, use `"Type": "Stars"`. Optional fields are `StarColor`,
`AccentColor` (`#RRGGBBAA`) and `Density` (0–1).
Omit `Backdrop` or use `"Type": "None"` for a plain background.

Distribute the JSON and artwork in the same folder structure, with any
required artwork licenses. Fonts remain a separate user setting.

</details>

## Custom cursors

Cursor selection is independent of themes. In Settings, choose **Cursor**,
adjust **Cursor size** live, or use **Default** for the normal pointer.
**Refresh cursors** rescans the folder and reloads the selected image.

Put PNGs directly in `Data/SFSE/Plugins/SFSEMenuFrameworkCursors`.
Each PNG appears in the dropdown. Use printable ASCII filenames up to 63
characters, images up to 512 pixels per side, and files up to 32 MiB.

An optional matching JSON file defines the image's base size and click point.
For `ring.png`, create `ring.json`:

```json
{
  "Size": [32, 32],
  "Hotspot": [0.5, 0.5]
}
```

Size defaults to 32×32 (1–256 per side), multiplied by the cursor-size slider
and UI scale. Hotspot values range from 0 to 1: top-left is `[0, 0]` (default),
center is `[0.5, 0.5]`. Metadata files must be at most 64 KiB.
Invalid or missing images fall back to the normal pointer with a warning, as does
invalid metadata; omitting metadata is valid. Resize and text pointers stay
standard, and gamepad navigation hides the cursor. Selection and scale persist
in the framework INI.

## Building from source

Requires Xmake 3.0.9+, MSVC with C++23 support, and the Windows SDK.

Builds must be signed. Set `SFSE_MF_SIGNING_CERT` to the thumbprint of a
CurrentUser/My certificate whose public key matches the adjacent SDK.
A missing or mismatched key stops the build. This is only needed to build the
framework, not to use the SDK in a client mod.

```powershell
git clone https://github.com/QTR-Modding/SFSE-MCP.git
git -C SFSE-MCP checkout 7a18b515ccdd325f3dd32564ffd892c2599f485d
git clone --recurse-submodules https://github.com/QTR-Modding/SFSE-Menu-Framework.git
cd SFSE-Menu-Framework
xmake f -m releasedbg
xmake
xmake package
```

The archive is `build/packages/SFSEMenuFramework-0.15.0.zip`.
To generate a Visual Studio solution, run `xmake project -k vsxmake`.

For a fork, `scripts/signing/Initialize-Signing.ps1` creates a non-exportable
key; pair it with your SDK's public-key setting. Do not replace an existing
signing key: clients built for it will reject the replacement.

## License and credits

[GPL-3.0-only](COPYING), with [exceptions](EXCEPTIONS) for original framework
code. SKSE Menu Framework-derived code remains GPL-3.0-only.
The separate client SDK is MIT-licensed.

See [third-party notices](THIRD_PARTY_NOTICES.md) for SkyrimThiago's source
revisions, other credits, and font and dependency licenses.
