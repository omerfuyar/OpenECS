# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Open questions

- **ui plugin.** Should a first-party ui plugin offer drawing and user-interface elements to other plugins? (OVERVIEW 3.3, DESIGN 5.4)
- **Other first-party plugins.** Which other plugins ship with OpenECS? (OVERVIEW 3.3)
- **Domain plugins.** Should plugins for specific domains, such as glTF models, audio or networking, live in their own repositories instead of being first-party? (OVERVIEW 3.3)
- **Build system.** The repository builds with shuild. Decide whether it stays, then add build and test commands to README.md.

## Tasks

- **Implement the rest of DESIGN.md.** Not done yet: GPU surfaces, pop-out windows, callbacks (`fn<...>`) in services, telling a service's users that its provider failed, the rest of the `ecs` table (`session`), opening panels in a new OS window, reopening the last closed panel and its parity list (DESIGN 11.5), restarting a faulted panel from its menu, the panel menu's entries that panel types add and its pop-out, split and move-to-workspace entries, saving a session to a file and opening one while OpenECS runs, popups, drag and drop of data, asking about unsaved work before another session is opened, and the first-party ui, settings and launcher plugins.
- **Link SDL as shared libraries.** DESIGN 17.2 ships SDL3 and SDL3_ttf as shared libraries next to the executable; the default build links them statically.
- **Rendering prototype.** The main window draws with a 2D GPU renderer. Still to confirm: one GPU device for several OS windows, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window, popups, and moving focus between OS windows (DESIGN 7.1, 7.2, 18).
- **Pin submodules to release tags** (DESIGN 17.4). They point to development snapshots: SDL 3.5.0-dev, SDL_ttf 3.3.0-dev, Lua v5.5.1 plus 6 commits, Clay v0.14 plus 99 commits. The design needs SDL 3.4.0 or later.
