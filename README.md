# SFSE Menu Framework

SFSE Menu Framework is a native Starfield Script Extender plugin for building ImGui menus directly in C++.

## Requirements

- Starfield 1.16.244
- [SFSE](https://sfse.silverlock.org/)
- Address Library for SFSE Plugins matching Starfield 1.16.244
- [Xmake](https://xmake.io/) 3.0.9 or newer
- A C++23-capable MSVC toolchain

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

SFSE Menu Framework is licensed under [GPL-3.0-or-later](COPYING) with the [Modding Exception and GPL-3.0 Linking Exception](EXCEPTIONS). Dear ImGui remains available under its [MIT license](extern/imgui/LICENSE.txt).
