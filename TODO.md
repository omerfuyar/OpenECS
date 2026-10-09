# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Open questions

- **Other first-party plugins.** Which other plugins ship with OpenECS? (OVERVIEW 3.3)
- **Domain plugins.** Should plugins for specific domains, such as glTF models, audio or networking, live in their own repositories instead of being first-party? (OVERVIEW 3.3)
- **Build system.** The repository builds with shuild. Decide whether it stays, then add build and test commands to README.md.

## Tasks

- **Implement the rest of DESIGN.md.** Not done yet: GPU surfaces, pop-out windows, opening panels in a new OS window, the panel menu's pop-out entry, popups, images and user-interface elements in the ui plugin, the first-party settings and launcher plugins, and the launcher preset, which OpenECS and tests start with when they name no preset (DESIGN 13.8, 17.5).
- **Releases for other platforms.** Shuild builds on Linux only. The release workflow has entries for Linux on aarch64, Windows and macOS, commented out until the build supports them (DESIGN 19.4).
- **Rendering prototype.** The main window draws with a 2D GPU renderer. Still to confirm: one GPU device for several OS windows, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window, popups, and moving focus between OS windows (DESIGN 7.1, 7.2, 18).
