# SFSE Menu Framework

SFSE Menu Framework is a native Starfield Script Extender plugin for building ImGui menus directly in C++.

## Requirements

- Starfield 1.16.244
- [SFSE](https://sfse.silverlock.org/)
- Address Library for SFSE Plugins matching Starfield 1.16.244
- [Xmake](https://xmake.io/) 3.0.9 or newer
- A C++23-capable MSVC toolchain

## Controls

- Press `F1` to open or close the Mod Control Panel.

## C++ consumer API

An SFSE plugin can register a page without installing its own renderer, window
hook, or input hook:

```cpp
#include <SFSEMenuFramework/SFSEMenuFramework.h>

void __stdcall RenderSettings()
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

Call the registration function from the SFSE `kPostLoad` message so it works
regardless of DLL load order. Consumer projects must compile the four Dear ImGui
core sources at version 1.90.8, commit
`6f7b5d0ee2fe9948ab871a530888a6dc5c960700`, and must not compile or initialize
an ImGui platform or renderer backend. The SDK header binds the consumer's ImGui
copy to the framework context and allocator for each callback.

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

The Starfield cursor, control-layer, simulation-pause, and native-main-thread queue ownership-transfer protocols are adapted from [OSF UI at commit `14b7565`](https://github.com/ozooma10/osf-ui/tree/14b7565bbc7689b07fdccdb74525b9505f9f0dd6) by ozooma10, under GPL-3.0 with its Modding and GPL-3.0 Linking Exceptions.
