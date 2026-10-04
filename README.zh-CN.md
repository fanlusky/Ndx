# Ndx

[English](README.md)

Ndx 是一款用 C++20 编写的 Windows 原生 Neovim 图形客户端。它使用 DirectWrite 和 Direct2D 绘制 Neovim 界面，并提供面向 Windows 的功能，例如 TSF 输入法支持、多字体回退，以及可选的光标和滚动动画。

Ndx 是基于早期 Nvy 代码发展而来的个人项目。本文末尾记录了项目来源，以及 Ndx 开始独立定制开发的提交。

## 演示

<!-- 将录制好的演示视频放到 assets/ndx-demo.mp4。 -->

[打开 Ndx 演示视频](assets/ndx-demo.mp4)

## 功能

- 使用 DirectWrite 和 Direct2D 原生绘制 Windows 界面
- 通过 Text Services Framework（TSF）支持输入法，包括行内组合文本和跟随光标的候选窗口
- `guifont` 支持逗号分隔的字体列表，可为缺失字形回退到其他字体
- 可选的光标动画、光标特效、平滑滚动和光标渐隐闪烁
- 使用 Alt+Enter 切换全屏；使用 Ctrl+鼠标滚轮缩放
- 拖放文件即可打开；按住 Ctrl 拖放可在新窗口打开
- 支持设置窗口位置、网格大小、字体和 Neovim 可执行文件的命令行参数

## 环境要求

- Windows
- Neovim（`nvim.exe`），建议使用较新的版本
- 构建环境：安装了 C++ 工作负载的 Visual Studio、CMake、Ninja 和较新的 Windows SDK

## 构建

在 PowerShell 中运行：

```powershell
git clone https://github.com/fanlusky/Ndx.git
cd Ndx
.\scripts\build.ps1                    # Release -> build\release\Ndx.exe
.\scripts\build.ps1 -Config Debug      # Debug -> build\debug\Ndx.exe
.\scripts\build.ps1 -Clean -Run        # 清理后重新构建并启动
```

如果当前尚未进入 MSVC x64 开发环境，脚本会自动设置。也可以直接使用 CMake 和 Ninja：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 使用与配置

Ndx 会启动 Neovim 作为编辑后端，并设置全局变量 `g:ndx = 1`。你可以在 Neovim 配置中用它判断当前是否运行在 Ndx 里，并应用专属设置。

例如，在 Neovim 配置中设置字体：

```vim
set guifont=CaskaydiaCove\ Nerd\ Font,Source\ Han\ Sans\ SC:h12
```

列表中第一个已安装的字体作为主字体，后续字体用于补齐缺失字形。`:h12` 用于指定字号。Lua 写法如下：

```lua
vim.opt.guifont = { "CaskaydiaCove Nerd Font", "Source Han Sans SC", ":h12" }
```

可用的命令行参数：

| 参数 | 说明 |
| --- | --- |
| `--maximize` | 启动时最大化窗口 |
| `--fullscreen` | 启动时进入全屏 |
| `--position=<x>,<y>` | 设置窗口初始位置，例如 `--position=500,200` |
| `--geometry=<cols>x<rows>` | 设置初始网格大小，例如 `--geometry=80x25` |
| `--disable-ligatures` | 禁用字体连字 |
| `--disable-fullscreen` | 禁用 Alt+Enter 全屏切换 |
| `--linespace-factor=<float>` | 设置行距，例如 `--linespace-factor=1.2` |
| `--cursor-timeout=<int>` | 光标空闲指定毫秒数后隐藏 |
| `--neovim-bin=<path>` | 指定 `nvim.exe` 的路径 |

## 可选动画与文字光晕

动画默认关闭。可在 Neovim 的 Lua 配置中启用：

```lua
vim.g.ndx_cursor_animation = true
vim.g.ndx_cursor_vfx_mode = "railgun" -- 也可以传入列表，例如 { "railgun", "sonicboom" }
vim.g.ndx_scroll_animation = true
vim.g.ndx_neon_text = true -- 文字霓虹光晕，默认关闭
vim.g.ndx_neon_radius = 4.0 -- 外层光晕范围，单位像素（0.5–40）
vim.g.ndx_neon_intensity = 1.0 -- 光晕强度（0–2）
```

Ndx 支持光标移动动画、光标粒子特效、平滑闪烁和平滑滚动。相关设置沿用 Neovide 的名称，并使用 `ndx_` 前缀。例如，`g:ndx_cursor_animation_length` 控制光标移动时长，`g:ndx_scroll_animation_length` 控制滚动时长。未设置的选项使用默认值。平滑滚动根据 Neovim 的窗口滚动事件执行；浮动窗口移动时不会播放动画。

将 `g:ndx_cursor_trail_in_insert_mode` 设为 `false` 后，打字时只保留平滑的光标移动，效果类似 Word 的平滑光标：insert 模式下光标移动不会拉出拖尾，也不显示粒子特效；普通模式仍保留完整动画。默认值为 `true`。打字时两个字符以内的移动使用 `g:ndx_cursor_short_animation_length`（默认 `0.04`），如需更明显的滑动效果，可调大到 `0.08` 等值。

设置 `g:ndx_neon_text` 为 `true` 或 `false` 可在运行时开关文字霓虹光晕，默认关闭。
`g:ndx_neon_radius` 控制模糊范围，默认 `4.0` 像素；`g:ndx_neon_intensity` 控制亮度，默认 `1.0`，设为 `0` 时没有光晕。两项都能在运行时调整，超出范围的值会被限制在允许范围内。

## 许可证与项目来源

Ndx 保留原项目的 MIT 许可证和版权声明，详见 [LICENSE](LICENSE)。本项目基于 [Rasmus Ishøy Michelsen 的 Nvy](https://github.com/RMichelsen/Nvy) 代码发展而来。

Ndx 的定制开发始于提交 [`3ffe894`](https://github.com/fanlusky/Ndx/commit/3ffe894e7e3c91cd408b3d265a89ff4ec93b337b)，提交说明为 **“Replace IMM32 IME handling with TSF”**，其父提交是 [`367190e`](https://github.com/fanlusky/Ndx/commit/367190ed748c189ee9f1d50c28569a6daa12b369)。随后在提交 [`7f5a7d6`](https://github.com/fanlusky/Ndx/commit/7f5a7d611b5c872e216bb1dd74e53b35971c393a) 中正式改名为 Ndx，提交说明为 **“Rename to Ndx, support guifont font lists, fix startup with file arguments.”**

原项目引用：[RMichelsen/Nvy](https://github.com/RMichelsen/Nvy)。
