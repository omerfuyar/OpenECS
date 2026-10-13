# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Tasks

- **Implement the rest of DESIGN.md.** Not done yet: GPU surfaces.
- **Releases for other platforms.** Shuild builds on Linux only. The release workflow has entries for Linux on aarch64, Windows and macOS, commented out until the build supports them (DESIGN 19.4).
- **Rendering prototype.** The main window draws with a 2D GPU renderer. Still to confirm: one GPU device for several OS windows, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window and into another one, popups, and moving focus between OS windows (DESIGN 6.10, 7.1, 7.2, 18). The tests run on SDL's offscreen driver, where windows have places on the screen.

## Reaching users

What helps people find OpenECS and stay. Before announcing a release widely:

- **A demo GIF** of 20 to 30 seconds at the top of README.md: dragging panels, popping one out, the settings window, and a plugin's code beside its panel.
- **Screenshots** of the examples for README.md and the release descriptions.
- **GitHub settings**, which the owner sets: topics (`sdl3`, `lua`, `editor`, `plugin-system`, `c23`, `tooling`), a social preview image, Discussions turned on, and issues labelled "good first issue".

Then announce in this order, each post written for its community: SDL's forum and the Handmade Network; r/C_Programming, r/lua, r/gamedev, r/linux and r/opensource; Show HN and Lobsters; the Lua mailing list. After a release, a short devlog, and a monthly "how I built X with OpenECS" post.

What would grow it most:

- **Releases for Windows and macOS**, since most tool builders are not on Linux (see Tasks).
- **A flagship tool** that is useful without writing code, such as a log viewer, a glTF viewer or a terminal, built from the standard plugins.
- **A showcase** in README.md of plugins and tools others build.
- **One place to talk**, Discord or Matrix, linked from README.md.

