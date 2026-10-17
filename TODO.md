# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Windows and macOS.** OpenECS builds on Windows with LLVM-MinGW, and the checks build and test it there. What is left: releases for Windows, and macOS. See "Releases for other platforms".
- **Shuild's issues.** The ones listed under Shuild.
- **Bugs and the reports from Hyprland.** The tasks below that wait to be confirmed there, and what the owner reports from testing.

## Tasks

- **Docs for every function's parameters before the first major release.** Each function of the core and of the standard plugins says what each parameter is and what it gives back: in OpenECS.h, ecs.lua and the signatures' docs. Functions are likely to change until then, so this waits for the release.
- **Releases for other platforms.** The release workflow has entries for Linux on aarch64, Windows and macOS, commented out until the build supports them (DESIGN 19.4). Windows builds now, with `.github/scripts/install-windows.ps1`; its release needs `package.sh` to make a zip with the DLLs, and a choice: whether the program opens a console window, as now, or is built with `-mwindows` and logs only to its file. macOS needs libffi's configuration for it, and its own linker flags.
- **TLS on Windows.** net and web check servers against a file of trusted certificates, which Windows does not have; `SSL_CERT_FILE` can name one. To read Windows' certificate store into mbedTLS, with crypt32.
- **Linker flags at compile time.** Shuild gives a module's flags to the compiler and the linker alike, so clang warns that `-Wl,` flags are unused when it compiles. Shuild could take linker flags of their own.
- **GPU panels on real drivers.** 21_gpu's test runs on Mesa's software Vulkan driver. The owner ran 21_gpu on an RTX 5070 with Hyprland, and it used the card. To confirm on X11, and with the panel moving between OS windows (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window and into another one, popups, moving focus between OS windows, and the title bar's moving and resizing (DESIGN 6.10, 6.11, 7.1, 7.2, 18). The tests run on SDL's offscreen driver, where windows have places on the screen. On CachyOS with Hyprland, message dialogs, such as the question about unsaved work, showed nothing and froze OpenECS; the core now draws them itself (DESIGN 7.7), which is to confirm there.
- **Pauses of a few seconds.** Reported on Hyprland and on openSUSE Tumbleweed. Vsync is off on Wayland now (DESIGN 3.1), which should end them; to confirm there.
- **GPU panels on a laptop with two GPUs.** On the owner's laptop with a GTX 1050, the GPU could not present to the window ("This surface does not support presenting!"), so GPU panels showed placeholders. Now the window shows copies of them, which the GPU downloads after each draw (DESIGN 5.3); to confirm there. Presenting with the integrated GPU, with `SDL_HINT_RENDER_GPU_LOW_POWER`, could make it faster. The app also used 70% of the CPU to show the placeholders; to find what redraws.
- **A crash when pop-out windows close or are tiled.** Reported on the same systems: a jump to a null address. It does not happen on X11 or on weston. Pop-out windows now share one GPU device and release their popups first; to confirm there, with the steps that cause it.

## Shuild

[Shuild](https://github.com/omerfuyar/shuild) is the build tool, a single header at `dependencies/shuild`. Shu, at `dependencies/shu`, holds the types and macros it uses. The owner allows changes to both, in their style, through pull requests into their main branches. These repositories have no CI, so a pull request is merged with a merge commit once it is tested locally: shuild's examples, and OpenECS built from scratch with its checks. Then an OpenECS pull request moves the submodule. Work found in it:

- **Windows and macOS.** Shuild names libraries for the system its compiler compiles for, and cross-compiles for Windows with MinGW (omerfuyar/shuild#23). Its Windows host code runs in OpenECS's Windows checks, which build OpenECS on Windows. `SHU_UtilRun` runs commands with the system's shell, which is `cmd.exe` on Windows.
- **Its own todos.** The header lists a module macro, like Clay's, and renaming the macros to `SHUILD_`.
- **Its issues that need the owner's design.** Finding the compilers on the system (omerfuyar/shuild#22), more variadic functions (omerfuyar/shuild#15) and finding other build scripts (omerfuyar/shuild#7), which `SHU_UtilBuildScript` covers in part.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **web.** HTTP/2 and compressed answers, which need more libraries or more of libcurl.
- **fs.** Watching folders on Windows and macOS reads each folder again every half second. `ReadDirectoryChangesW` on Windows and FSEvents on macOS would tell changes as they happen.
- **gpu: shaders from source.** gpu takes compiled shaders: SPIR-V, DXIL or MSL, the one the GPU's driver takes, so a plugin that runs on several systems ships each. SDL_shadercross could compile one source, such as HLSL or SPIR-V, to the others at run time, but it would be a new dependency, with its own: SPIRV-Cross, and DXC for DXIL. To decide: whether gpu takes source, and so the dependency, or plugins keep compiling their shaders before they ship, as 22_gpu_model does with glslang. Set aside for now; more important work comes first.
- **math.** Plugins of math functions that other plugins share, from Lua and C: vectors, matrices and the like. The owner wants them later.
- **gpu: text in pipelines.** `gpu.text` draws flat text, as draw does. A plugin's own shaders cannot draw text yet, such as labels in a 3D scene: gpu could give the textures of the text engine's glyphs and the rectangles of a text in them.

## Questions

- **Desktop entries.** A preset gives the tool's identity (DESIGN 13.2). It could also describe the tool's entry in the system's application launcher: its categories, and the file types its `open` function takes, so installing a tool adds it to the launcher and to "Open with". To decide: whether presets describe it, and whether OpenECS or an installer writes the `.desktop` file. The owner decides it later.

## Reaching users

What helps people find OpenECS and stay. Before announcing a release widely:

- **A demo GIF** of 20 to 30 seconds at the top of README.md: dragging panels, popping one out, the settings window, and a plugin's code beside its panel.
- **Screenshots** of the examples for README.md and the release descriptions.
- **GitHub settings**, which the owner sets for each of OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template:
  - a description, one line on what the repository holds;
  - topics, such as `sdl3`, `lua`, `editor`, `plugin-system`, `c23` and `tooling`;
  - a social preview image, such as a screenshot of a tool with its panels;
  - issues labelled "good first issue".

Then announce in this order, each post written for its community: SDL's forum and the Handmade Network; r/C_Programming, r/lua, r/gamedev, r/linux and r/opensource; Show HN and Lobsters; the Lua mailing list. After a release, a short devlog, and a monthly "how I built X with OpenECS" post.

What would grow it most:

- **Releases for Windows and macOS**, since most tool builders are not on Linux (see Tasks).
- **A flagship tool** that is useful without writing code, such as a log viewer, a glTF viewer or a terminal, built from the standard plugins.
- **A showcase** in README.md of plugins and tools others build.
- **One place to talk**, Discord or Matrix, linked from README.md.

