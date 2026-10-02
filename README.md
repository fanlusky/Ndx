# Ndx

Ndx is a fork of [Nvy](https://github.com/RMichelsen/Nvy) by Rasmus Ishøy Michelsen, a minimal
[Neovim](https://neovim.io/) client for Windows written in C++. All credit for the original client goes to
its author and contributors; it is distributed under the MIT license, see [LICENSE](LICENSE).

Changes compared to Nvy:

- IME support through the
  [Text Services Framework (TSF)](https://learn.microsoft.com/en-us/windows/win32/tsf/text-services-framework)
  instead of IMM32, see [Input Method (TSF)](#input-method-tsf)
- `guifont` accepts a comma separated list of fonts, the extra fonts are used for glyphs missing from
  the main font (e.g. Nerd Font icons, CJK characters)
- The default window is 2/3 as wide
- The executable is `Ndx.exe` and the global vim variable is `g:ndx` (instead of `g:nvy`)

Like Nvy, Ndx uses DirectWrite to shape and render the grid cells and text.\
Since Ndx is just a front-end for Neovim, installing Neovim is required to use Ndx, preferably the
latest nightly version from [here](https://github.com/neovim/neovim/releases).

![Showcase image](resources/client.png)

## Configuration

Ndx sets the global vim variable `g:ndx = 1` in case you want to specialize your init.vim while using Ndx.

Fonts can be changed by setting the guifont in `init.vim`, for example:
`set guifont=Fira\ Code:h24`. <br>
If no font size is given, the current size is kept. <br>
Several fonts can be listed, separated by commas. The first one which is installed is the main font, the
following ones are used for glyphs the main font doesn't have, then the system fallback fonts. For example,
with icons from a Nerd Font and Chinese characters from Source Han Sans:
`set guifont=CaskaydiaCove\ Nerd\ Font,Source\ Han\ Sans\ SC:h12`, or in Lua
`vim.opt.guifont = { "CaskaydiaCove Nerd Font", "Source Han Sans SC", ":h12" }`. <br>
The Nvy syntax `set guifont=Fira\ Code:h24:Consolas` still works, Consolas being one more font of the list.

Ndx can be started with the following flags:
- `--maximize` to start in maximized
- `--fullscreen` to start in fullscreen
- `--position=<x>,<y>` to start with a given position, e.g. `--position=500,200`
- `--geometry=<cols>x<rows>` to start with a given number of rows and columns, e.g. `--geometry=80x25`
- `--disable-ligatures` to disable font ligatures
- `--disable-fullscreen` to disable toggling fullscreen with Alt+Enter
- `--linespace-factor=<float>` to scale the line spacing by a floating point factor, e.g. `--linespace-factor=1.2`
- `--cursor-timeout=<int>` to hide the cursor after some time (in ms) of being idle, e.g. `--cursor-timeout=2000`
- `--neovim-bin=<path>` to provide path to nvim.exe, e.g. `--neovim-bin="C:\neovim\nvim-win64\bin\nvim.exe"`

## Extra Features

- You can use Alt+Enter to toggle fullscreen
- You can use Ctrl+Mousewheel to zoom
- You can drag files onto Ndx to open them (:e)
- Dragging files while holding Ctrl opens them in a new window (:new)

## Input Method (TSF)

Ndx is a TSF-aware application, input methods (e.g. Microsoft Pinyin, Japanese IME) talk to it through TSF:

- The composition string is drawn inline at the cursor, using the underline style (solid, dotted, dashed,
  squiggle, bold) and colors declared by the input method's display attributes
- The candidate window follows the cursor, including in the command line
- Committed text is sent to Neovim as soon as the input method finalizes it
- Clicking with the mouse commits the current composition first

## Releases

Releases of the original Nvy can be found [here](https://github.com/RMichelsen/Nvy/releases)

## Build

### Requirements

- A compiler supporting `C++20`
- [The latest Windows SDK](https://developer.microsoft.com/en-us/windows/downloads/windows-10-sdk/)

Apart from the Windows SDK, the only dependency Ndx uses is the excellent [MPack](https://github.com/ludocode/mpack) library
which is compiled alongside the client itself.

### Build with the PowerShell script

Requires Visual Studio with the C++ workload, [CMake](https://cmake.org/) and [Ninja](https://ninja-build.org/).
The script sets up the MSVC x64 environment by itself.

```powershell
git clone https://github.com/fanlusky/Ndx.git
cd Ndx
.\scripts\build.ps1                    # Release -> build\release\Ndx.exe
.\scripts\build.ps1 -Config Debug      # Debug   -> build\debug\Ndx.exe
.\scripts\build.ps1 -Clean -Run        # Rebuild from scratch, then launch
```

### Build example with [Ninja](https://ninja-build.org/)

```sh
git clone https://github.com/fanlusky/Ndx.git
cd Ndx
mkdir build
cd build
cmake .. -GNinja
ninja
```
