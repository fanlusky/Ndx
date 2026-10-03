# Ndx

[简体中文](README.zh-CN.md)

Ndx is a native Windows graphical client for [Neovim](https://neovim.io/), written in C++20. It renders Neovim's UI with DirectWrite and Direct2D and adds Windows-focused features such as TSF input method support, font fallback lists, and optional cursor and scrolling animations.

Ndx is a personal project based on the earlier Nvy codebase. Its history and the point where Ndx-specific development began are documented at the end of this README.

## Demo

https://github.com/user-attachments/assets/3d2d0769-f280-46a9-afdd-c4c6d9838f6c

## Features

- Native Windows rendering with DirectWrite and Direct2D
- Text Services Framework (TSF) support for IMEs, including inline composition text and a cursor-following candidate window
- Comma-separated `guifont` font lists, with fallback fonts for missing glyphs
- Optional animated cursor, cursor effects, smooth scrolling, and smooth cursor blinking
- Alt+Enter to toggle fullscreen; Ctrl+mouse wheel to zoom
- Drag and drop files to open them; hold Ctrl while dropping to open them in a new window
- Command-line options for window placement, geometry, fonts, and the Neovim executable

## Requirements

- Windows
- Neovim (`nvim.exe`), preferably a recent release
- To build: Visual Studio with the C++ workload, CMake, Ninja, and a recent Windows SDK

## Build

From PowerShell:

```powershell
git clone https://github.com/fanlusky/Ndx.git
cd Ndx
.\scripts\build.ps1                    # Release -> build\release\Ndx.exe
.\scripts\build.ps1 -Config Debug      # Debug -> build\debug\Ndx.exe
.\scripts\build.ps1 -Clean -Run        # Clean build, then launch
```

The script enters the MSVC x64 developer environment when needed. You can also configure and build with CMake and Ninja directly:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Usage and configuration

Ndx starts Neovim as its editing backend. It sets `g:ndx = 1`, which you can use to customize your Neovim configuration when running inside Ndx.

Set the font in your Neovim configuration, for example:

```vim
set guifont=CaskaydiaCove\ Nerd\ Font,Source\ Han\ Sans\ SC:h12
```

The first installed font is used as the main font; later fonts provide glyph fallback. The font size can be specified with `:h12`. The Lua equivalent is:

```lua
vim.opt.guifont = { "CaskaydiaCove Nerd Font", "Source Han Sans SC", ":h12" }
```

Available command-line options:

| Option | Description |
| --- | --- |
| `--maximize` | Start maximized |
| `--fullscreen` | Start fullscreen |
| `--position=<x>,<y>` | Set the initial window position, for example `--position=500,200` |
| `--geometry=<cols>x<rows>` | Set the initial grid size, for example `--geometry=80x25` |
| `--disable-ligatures` | Disable font ligatures |
| `--disable-fullscreen` | Disable the Alt+Enter fullscreen toggle |
| `--linespace-factor=<float>` | Set line spacing, for example `--linespace-factor=1.2` |
| `--cursor-timeout=<int>` | Hide the cursor after the given idle time in milliseconds |
| `--neovim-bin=<path>` | Use a specific `nvim.exe` path |

## Optional animations and text glow

Animations are off by default. Enable them in your Neovim Lua configuration:

```lua
vim.g.ndx_cursor_animation = true
vim.g.ndx_cursor_vfx_mode = "railgun" -- or a list, e.g. { "railgun", "sonicboom" }
vim.g.ndx_scroll_animation = true
vim.g.ndx_neon_text = true -- glow around text; disabled by default
vim.g.ndx_neon_radius = 4.0 -- outer glow radius in pixels (0.5–40)
vim.g.ndx_neon_intensity = 1.0 -- glow strength (0–2)
```

Ndx supports cursor movement animation, cursor particle effects, smooth cursor blinking, and smooth scrolling. The settings use the corresponding Neovide names with an `ndx_` prefix. For example, `g:ndx_cursor_animation_length` controls cursor movement duration and `g:ndx_scroll_animation_length` controls scroll duration. Unset settings use their defaults. Smooth scrolling follows Neovim window scroll events; moving floating windows are not animated.

Set `g:ndx_neon_text` to `true` or `false` to toggle the text glow while Ndx is running. It is off by default.
`g:ndx_neon_radius` controls the blur range in pixels (default `4.0`); `g:ndx_neon_intensity` controls brightness (default `1.0`, with `0` removing the glow). Both can be changed while Ndx is running; out-of-range values are clamped.

## License and project history

Ndx retains the original project's MIT license and copyright notice; see [LICENSE](LICENSE). The upstream project that this codebase builds on is [Nvy by Rasmus Ishøy Michelsen](https://github.com/RMichelsen/Nvy).

Ndx-specific development began at commit [`3ffe894`](https://github.com/fanlusky/Ndx/commit/3ffe894e7e3c91cd408b3d265a89ff4ec93b337b), **“Replace IMM32 IME handling with TSF”**, whose parent is [`367190e`](https://github.com/fanlusky/Ndx/commit/367190ed748c189ee9f1d50c28569a6daa12b369). The project was renamed to Ndx in the following commit, [`7f5a7d6`](https://github.com/fanlusky/Ndx/commit/7f5a7d611b5c872e216bb1dd74e53b35971c393a), **“Rename to Ndx, support guifont font lists, fix startup with file arguments.”**

Original project reference: [RMichelsen/Nvy](https://github.com/RMichelsen/Nvy).
