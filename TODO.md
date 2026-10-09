# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Open questions

- **Other first-party plugins.** Which other plugins ship with OpenECS? (OVERVIEW 3.3)
- **Domain plugins.** Should plugins for specific domains, such as glTF models, audio or networking, live in their own repositories instead of being first-party? (OVERVIEW 3.3)

## Tasks

- **Implement the rest of DESIGN.md.** Not done yet: GPU surfaces.
- **Releases for other platforms.** Shuild builds on Linux only. The release workflow has entries for Linux on aarch64, Windows and macOS, commented out until the build supports them (DESIGN 19.4).
- **Rendering prototype.** The main window draws with a 2D GPU renderer. Still to confirm: one GPU device for several OS windows, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window and into another one, popups, and moving focus between OS windows (DESIGN 6.10, 7.1, 7.2, 18). The tests run on SDL's offscreen driver, where windows have places on the screen.
