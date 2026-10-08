# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Open questions

- **ui plugin.** Should a first-party ui plugin offer drawing and user-interface elements to other plugins? (OVERVIEW 3.3, DESIGN 5.4)
- **Other first-party plugins.** Which other plugins ship with OpenECS? (OVERVIEW 3.3)
- **Domain plugins.** Should plugins for specific domains, such as glTF models, audio or networking, live in their own repositories instead of being first-party? (OVERVIEW 3.3)
- **Plugin events.** Should the core provide plugin events, or should plugins build them themselves, for example with services and callbacks? (OVERVIEW 9.2, DESIGN 8)
- **Plugin state.** Should the core save plugin state into the session, or should plugins save their own data? (OVERVIEW 9.2, DESIGN 13.2, 13.3)
- **Value allocation.** Every node of a value, its strings and its arrays are allocated on their own with SDL's allocator. Values are built at start-up, when a session is saved, when settings change and on service calls with `value` arguments, never on every frame. One option is an arena for each value tree, freed at once, and a scratch arena for the values of one call. Recommendation: keep SDL's allocator, measure a service call with a large value first, and add the arenas only if the measurement shows a cost. (DESIGN 10.3)
- **Build system.** The repository builds with shuild. Decide whether it stays, then add build and test commands to README.md.

## Tasks

- **Implement the rest of DESIGN.md.** Not done yet: GPU surfaces, pop-out windows, core events (DESIGN 8.1), events for plugins, callbacks (`fn<...>`) in services, telling a service's users that its provider failed, the rest of the `ecs` table (`event`, `session`), opening panels in a new OS window, reopening the last closed panel and its parity list (DESIGN 11.5), restarting a faulted panel from its menu, saving a session to a file and opening one while OpenECS runs, popups, drag and drop of data, asking about unsaved work before another session is opened, and the first-party ui, settings and launcher plugins.
- **Link SDL as shared libraries.** DESIGN 17.2 ships SDL3 and SDL3_ttf as shared libraries next to the executable; the default build links them statically.
- **Rendering prototype.** The main window draws with a 2D GPU renderer. Still to confirm: one GPU device for several OS windows, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window, popups, and moving focus between OS windows (DESIGN 7.1, 7.2, 18).
- **Pin submodules to release tags** (DESIGN 17.4). They point to development snapshots: SDL 3.5.0-dev, SDL_ttf 3.3.0-dev, Lua v5.5.1 plus 6 commits, Clay v0.14 plus 99 commits. The design needs SDL 3.4.0 or later.
