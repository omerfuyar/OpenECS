# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Shuild.** The work listed under Shuild. Its issues are closed: compiler detection, variadic functions, and build scripts in other folders, which `SHU_UtilBuildScript` covers.
- **Bugs and the reports from Hyprland.** The tasks below that wait to be confirmed there, and what the owner reports from testing.

## Tasks

- **Docs for every function's parameters before the first major release.** Each function of the core and of the standard plugins says what each parameter is and what it gives back: in OpenECS.h, ecs.lua and the signatures' docs. Functions are likely to change until then, so this waits for the release.
- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags. The Windows release is to try on a Windows computer: unpacking the zip, opening `OpenECS.exe`, and the log in `%APPDATA%\openecs\`.
- **Builds on the releases of OpenECS-std and OpenECS-examples, from 0.5.0.** Their releases hold only their source now. From 0.5.0, each also has its builds for Linux and Windows, made by its own release workflow: std's plugins, and the examples with their native plugins, built with that release of OpenECS (DESIGN 19.4).
- **Import the rulesets.** The rulesets of OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template require the Windows checks now, as the owner chose. The owner imports them in each repository's settings. OpenECS-examples' rulesets no longer require `Release (Linux)`, which it does not run.
- **GPU panels on real drivers.** 21_gpu's test runs on Mesa's software Vulkan driver. The owner ran 21_gpu on an RTX 5070 with Hyprland, and it used the card. To confirm on X11, and with the panel moving between OS windows (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window and into another one, popups, moving focus between OS windows, and the title bar's moving and resizing (DESIGN 6.10, 6.11, 7.1, 7.2, 18). The tests run on SDL's offscreen driver, where windows have places on the screen, and on headless sway, which tiles them. On Hyprland, the question about unsaved work showed for one frame when a window was closed, and a panel alone in a pop-out window could not be dragged into the main window; both are fixed, and to confirm there.
- **Pauses of a few seconds.** Reported on Hyprland and on openSUSE Tumbleweed. Vsync is off on Wayland now (DESIGN 3.1), which should end them; to confirm there.
- **GPU panels on a laptop with two GPUs.** On the owner's laptop with a GTX 1050, the GPU could not present to the window ("This surface does not support presenting!"), so GPU panels showed placeholders. Now the window shows copies of them, which the GPU downloads after each draw (DESIGN 5.3); to confirm there. Presenting with the integrated GPU, with `SDL_HINT_RENDER_GPU_LOW_POWER`, could make it faster. The app also used 70% of the CPU to show the placeholders: a placeholder asked for frames without end, which is fixed; to confirm there.

## Shuild

[Shuild](https://github.com/omerfuyar/shuild) is the build tool, a single header at `dependencies/shuild`. Shu, at `dependencies/shu`, holds the types and macros it uses. The owner allows changes to both, in their style, through pull requests into their main branches. These repositories have no CI, so a pull request is merged with a merge commit once it is tested locally: shuild's examples, and OpenECS built from scratch with its checks. Then an OpenECS pull request moves the submodule. Work found in it:

- **Windows and macOS.** Shuild names libraries for the system its compiler compiles for, and cross-compiles for Windows with MinGW (omerfuyar/shuild#23). Its Windows host code runs in OpenECS's Windows checks, which build OpenECS on Windows. `SHU_UtilRun` runs commands with the system's shell, which is `cmd.exe` on Windows.
- **Its own todo.** The header lists a module macro, like Clay's.
- **Compiler detection on Windows.** `SHU_CompilerDetect` lists the compilers on PATH, and `SHU_CompilerTryConfigure(NULL)` picks one (omerfuyar/shuild#22). Its Windows code, which lists `.exe` files, is compiled but not run on Windows yet.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **web.** HTTP/2 and compressed answers, which need more libraries or more of libcurl.
- **fs.** Watching folders on Windows and macOS reads each folder again every half second. `ReadDirectoryChangesW` on Windows and FSEvents on macOS would tell changes as they happen.
- **gpu: shaders from source.** gpu takes compiled shaders: SPIR-V, DXIL or MSL, the one the GPU's driver takes, so a plugin that runs on several systems ships each. SDL_shadercross could compile one source, such as HLSL or SPIR-V, to the others at run time, but it would be a new dependency, with its own: SPIRV-Cross, and DXC for DXIL. To decide: whether gpu takes source, and so the dependency, or plugins keep compiling their shaders before they ship, as 22_gpu_model does with glslang. Set aside for now; more important work comes first.
- **math.** Plugins of math functions that other plugins share, from Lua and C: vectors, matrices and the like. The owner wants them later.

## Questions

- **Desktop entries.** A preset gives the tool's identity (DESIGN 13.2). It could also describe the tool's entry in the system's application launcher: its categories, and the file types its `open` function takes, so installing a tool adds it to the launcher and to "Open with". To decide: whether presets describe it, and whether OpenECS or an installer writes the `.desktop` file. The owner decides it later.

## Reaching users

What helps people find OpenECS and stay. Before announcing a release widely:

- **GIFs of what OpenECS can do** in README.md, which the owner records.
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

