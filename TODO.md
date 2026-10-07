# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Open questions

- **ui plugin.** Should a first-party ui plugin offer drawing and user-interface elements to other plugins? (OVERVIEW 3.3, DESIGN 5.5)
- **Other first-party plugins.** Which other plugins ship with OpenECS? (OVERVIEW 3.3)
- **Domain plugins.** Should plugins for specific domains, such as glTF models, audio or networking, live in their own repositories instead of being first-party? (OVERVIEW 3.3)
- **Plugin events.** Should the core provide plugin events, or should plugins build them themselves, for example with services and callbacks? (OVERVIEW 9.2, DESIGN 8)
- **Plugin state.** Should the core save plugin state into the session, or should plugins save their own data? (OVERVIEW 9.2, DESIGN 13.2, 13.3)
- **Build system.** The repository builds with shuild. Decide whether it stays, then add build and test commands to README.md.
- **shutil in the core.** The core needs dynamic arrays and hash maps. [shutil](https://github.com/omerfuyar/shutil) provides arrays and plans the rest. Using it adds a dependency.

## Tasks

- **Rendering prototype.** One GPU device for several OS windows, a 2D GPU renderer per window, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.5).
- **libffi closure prototype.** Expose a Lua function as a typed C function pointer (DESIGN 10.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window, popups, and moving focus between OS windows (DESIGN 7.1, 7.2, 18).
- **Pin submodules to release tags** (DESIGN 17.4). They point to development snapshots: SDL 3.5.0-dev, SDL_ttf 3.3.0-dev, Lua v5.5.1 plus 6 commits, Clay v0.14 plus 99 commits. The design needs SDL 3.4.0 or later.
- **Align README.md's description with OVERVIEW.md.** It lists a file explorer, notifications, undo and redo, and animations as OpenECS features; OVERVIEW.md leaves them to plugins or out of the design.
