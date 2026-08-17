# Vape V4 ImGui - Pre-External Source

Source snapshot reconstructed from the project state captured at
2026-08-17 11:25 KST, immediately before Roblox external/runtime integration.

## Build

Requirements:

- Windows x64
- Visual Studio with Desktop development with C++
- Windows SDK and DirectX 11 libraries

Run:

```bat
build.bat build
```

Output:

```text
build\VapeV4.exe
```

`build.bat` without arguments builds and runs the application.

## Contents

- Vape GUI C++ source
- Standalone Win32 + DirectX 11 application shell
- Dear ImGui 1.90.4 minimal source and Win32/DX11 backends
- PNG assets
- stb_image

No Roblox external, runtime, process-memory, probe, or user config files are included.
Dear ImGui is distributed under the MIT license in `imgui/LICENSE.txt`.
<img width="1483" height="837" alt="image" src="https://github.com/user-attachments/assets/389a1858-6f00-4e34-ae9e-095d10b75505" />

## Preview
