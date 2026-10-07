# OpenECS TODO

What is not decided yet, and work that is waiting. When an item is settled, write the decision into [OVERVIEW.md](OVERVIEW.md) or [DESIGN.md](DESIGN.md) and remove it from here.

## Proposals waiting for the owner

Everything marked **Proposed** in OVERVIEW.md or **[Proposed]** in DESIGN.md waits for a decision. These are the ones that change how the product behaves:

- **Core prefix key** (OVERVIEW 9.4, DESIGN 7.5). One reserved key combination, then one key for a core action. This changes the decided rule "no key sequences in the core".
- **Grip** on panels that have no tab row (OVERVIEW 8.2, DESIGN 6.4).
- **Focus details:** `click` as the default, and how hover mode behaves (OVERVIEW 9.1, DESIGN 7.1).
- **Default place** for panels opened by code (OVERVIEW 8.5, DESIGN 6.6).
- **Panel menus** (OVERVIEW 8.8) and **tab row details** (OVERVIEW 8.2).
- **Operating-system integration:** what the core provides and what plugins provide (OVERVIEW 13).
- **Undo and redo** stay in panels and plugins (OVERVIEW 4.6).
- **Theme** as a few settings (OVERVIEW 4.6).
- **First-party plugins:** a launcher, a settings plugin written in Lua, and domain plugins kept out of this repository (OVERVIEW 4.4).
- **Command line** (DESIGN 13.5).
- **User settings** per tool, and extra plugins named in user settings (OVERVIEW 12.4, DESIGN 12.4).
- **What plugins may observe and how they name things** (OVERVIEW 10.4, DESIGN 8.3, 9.9).

Notable technical proposals:

- Pixel format: premultiplied `ARGB8888` (DESIGN 5.2).
- How the ui plugin draws (DESIGN 5.5).
- SDL3 and SDL3_ttf as shared libraries next to the executable (DESIGN 9.8, 17.2).
- C23 for the core, C11 for the plugin header (DESIGN 1, 17.3).
- Manifest format and version rule (DESIGN 9.2).
- File locations (DESIGN 16).
- Error convention and logging (DESIGN 14).

## Open questions

- **Build system.** The repository builds with shuild. Decide whether it stays, then add build and test commands to README.md.
- **Naming conventions.** The owner will provide them: case style, file names, and how plugins name their own things beyond the prefix rule (DESIGN 9.9).

## Tasks

- **Rendering prototype.** One GPU device for several OS windows, a 2D GPU renderer per window, a panel's GPU texture shown in any window, and an offscreen renderer drawing into that texture (DESIGN 5.3, 5.5).
- **libffi closure prototype.** Expose a Lua function as a typed C function pointer (DESIGN 10.4).
- **Wayland test on Hyprland.** Dragging a panel out of its window, popups, and moving focus between OS windows (DESIGN 7.1, 7.2, 19).
- **Pin submodules to release tags** (DESIGN 17.4). Today they point to development snapshots: SDL 3.5.0-dev, SDL_ttf 3.3.0-dev, Lua v5.5.1 plus 6 commits, Clay v0.14 plus 99 commits. Every SDL feature the design relies on exists in SDL 3.4.0. Desktop notifications (`SDL_notification.h`) arrive in SDL 3.6.0 and are only needed by a plugin.
- **Align README.md's description with OVERVIEW.md.** It still mentions cross-platform support, a file explorer, notifications, undo and redo, and themes. OVERVIEW.md leaves these out of the core or out of the design.
