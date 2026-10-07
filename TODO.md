# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Proposals waiting for the owner

- **Domain plugins outside this repository** (OVERVIEW 4.4). Plugins for specific domains, such as glTF, audio or networking, would live in their own repositories. This is about how you want to organize and maintain the project.

## Open questions

- **Build system.** The repository builds with shuild. Decide whether it stays, then add build and test commands to README.md.

## Tasks

- **Rendering prototype.** One GPU device for several OS windows, a 2D GPU renderer per window, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.5).
- **libffi closure prototype.** Expose a Lua function as a typed C function pointer (DESIGN 10.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window, popups, and moving focus between OS windows (DESIGN 7.1, 7.2, 19).
- **Pin submodules to release tags** (DESIGN 17.4). Today they point to development snapshots: SDL 3.5.0-dev, SDL_ttf 3.3.0-dev, Lua v5.5.1 plus 6 commits, Clay v0.14 plus 99 commits. Every SDL feature the design relies on exists in SDL 3.4.0. Desktop notifications (`SDL_notification.h`) arrive in SDL 3.6.0 and are only needed by a plugin.
- **Align README.md's description with OVERVIEW.md.** It still mentions cross-platform support, a file explorer, notifications, undo and redo, and themes. OVERVIEW.md leaves these out of the core or out of the design.
