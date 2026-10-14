# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Tasks

- **Implement the rest of DESIGN.md.** Not done yet: GPU surfaces.
- **Releases for other platforms.** Shuild builds on Linux only. The release workflow has entries for Linux on aarch64, Windows and macOS, commented out until the build supports them (DESIGN 19.4). README.md's list of packages is for Linux only; Windows needs its own: the compiler (LLVM or MinGW), cmake and ninja from winget, and what libffi's build needs in place of autoreconf.
- **Portable builds of std and examples.** Their `shuild.c` lists folders with `dirent.h`, which Windows does not have. Shuild has no function that lists a folder; one in shuild would let the builds use shuild and the standard library only. Left for later.
- **Rendering prototype.** Every OS window draws with a 2D renderer on one shared GPU device. Still to confirm: a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window and into another one, popups, moving focus between OS windows, and the title bar's moving and resizing (DESIGN 6.10, 6.11, 7.1, 7.2, 18). The tests run on SDL's offscreen driver, where windows have places on the screen.
- **Pauses of a few seconds.** Reported on Hyprland and on openSUSE Tumbleweed. Vsync is off on Wayland now (DESIGN 3.1), which should end them; to confirm there.
- **A crash when pop-out windows close or are tiled.** Reported on the same systems: a jump to a null address. It does not happen on X11 or on weston. Pop-out windows now share one GPU device and release their popups first; to confirm there, with the steps that cause it.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **ui for plugins in C.** ui's callbacks are Lua functions. A C plugin cannot give one, and ui cannot call a C plugin's functions by name, because a plugin uses only the functions of the plugins it depends on (DESIGN 10.5).
- **ui elements.** Trees, tables, tabs, tooltips, a number field, and selecting text in a field.
- **draw.** Images rotated or with a colour multiplied in.
- **image.** GIF animation, and writing images to files.
- **mixer.** Streaming long sounds instead of keeping them whole, and effects such as panning.
- **net.** TLS servers, beside its TLS clients.
- **web.** HTTP/2, compressed answers and WebSockets, which need more libraries or more of libcurl.
- **gltf.** Images inside a `.glb` file, skins and animations.
- **textgrid.** Bold and underlined cells, and wide characters that take two cells.
- **fs.** Watching a folder for changes.

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

