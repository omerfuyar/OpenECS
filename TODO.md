# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Open questions

- **ui plugin.** Should a first-party ui plugin offer drawing and user-interface elements to other plugins? (OVERVIEW 3.3, DESIGN 5.4)
- **Other first-party plugins.** Which other plugins ship with OpenECS? (OVERVIEW 3.3)
- **Domain plugins.** Should plugins for specific domains, such as glTF models, audio or networking, live in their own repositories instead of being first-party? (OVERVIEW 3.3)
- **Lua plugins of several files.** Should a plugin's `require` find modules in the plugin's folder and run them in the plugin's environment, so they can also `require("ecs")`? Now it gives Lua's `require` every name but `"ecs"`. (DESIGN 9.6)
- **How the launcher draws its list.** Its panel shows text, and no plugin can draw text yet. Should it wait for the ui plugin, or draw its text itself into a pixels surface for now? (DESIGN 5.4, 13.8)
- **Build system.** The repository builds with shuild. Decide whether it stays, then add build and test commands to README.md.

## Tasks

- **Implement the rest of DESIGN.md.** Not done yet: GPU surfaces, pop-out windows, opening panels in a new OS window, the panel menu's pop-out entry, popups, drag and drop of data, and the first-party ui, settings and launcher plugins.
- **Releases for other platforms.** Shuild builds on Linux only. The release workflow has entries for Linux on aarch64, Windows and macOS, commented out until the build supports them (DESIGN 19.4).
- **Link SDL as shared libraries.** DESIGN 17.2 ships SDL3 and SDL3_ttf as shared libraries next to the executable; the default build links them statically.
- **Rendering prototype.** The main window draws with a 2D GPU renderer. Still to confirm: one GPU device for several OS windows, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window, popups, and moving focus between OS windows (DESIGN 7.1, 7.2, 18).
