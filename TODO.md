# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Bugs** that the owner reports from testing.

## Tasks

- **SDL's tray closes the D-Bus connection of notifications.** On Linux, SDL's D-Bus tray calls `SDL_DBus_Quit` when its last tray goes, which closes the connection that SDL's notifications share: their clicks stop coming after notify's last tray goes. At exit, `SDL_CleanupNotifications` then removes a filter from a new connection, which libdbus aborts on; the core makes that a warning for its quit (`ECSIApp_QuitSDL`), so the workaround can go once SDL is fixed. It is SDL's bug, still in its `main`, for the owner to report: SDL takes no contributions written by AI.
- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now, and aarch64 waits for 1.0.0 (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags.
- **LuaJIT's part 2, the core's redesign.** OpenECS runs on LuaJIT 2.1 (DESIGN 9.6), through the C API. Measured in a Release build on Linux, with settings, keys, files and console redrawn whole 50 times a second, their Lua code took about 170 ms a second on LuaJIT, against 325 ms on Lua 5.5. A call through the C API ends the JIT's trace, and the FFI calls C from compiled code at C's speed. So the core changes where LuaJIT does a thing better, measured before and after with `ecs.profile` and LuaJIT's profiler. Each step is its own interface change (the interface-change skill), merged in the order of the pull-requests skill.
  - **What LuaJIT 2.1 has** is in DESIGN 9.6. Read its current `doc/extensions.html` before each step; `v2.1` gains features often. It has no `//`, no metamethods for the bit operators, and no compound assignment that changes its operand in place; if it gains them, use them.
    1. **Buffers from the FFI.** A `buffer` parameter also takes a cdata pointer with its length, and a `string.buffer`, without a copy (DESIGN 10.7). Plugins then build vertices and pixels in FFI arrays.
    2. **Structs as cdata.** A struct type is already laid out as C (DESIGN 10.11): `ffi.cdef` declares it, and `ffi.metatype` gives it its methods and operators. A function takes the cdata by pointer, without converting fields; a table of fields still works, through `ffi.new`. The operators add `mod`, `pow` and `concat`. Saved state and values (DESIGN 10.3, 13.4) write a struct as a table of its fields.
    3. **Workers' values through `string.buffer`.** `send` and `receive` encode and decode values with `string.buffer` in place of the copy in `Workers.c`; FFI cdata such as structs cross as well. A function or a handle in a value is an error, not skipped (DESIGN 3.4).
    4. **Lua's functions in the profile.** While `ecs.profile` is on, the core runs LuaJIT's sampling profiler on the main state, and `ecs.profile.get()` adds `functions`: the Lua functions where the last second went, with their plugin (DESIGN 3.5). std's profiler shows them.
    5. **ui.** With the tools above, rebuild ui's tree with `table.new` and reused tables (`table.clear`), and check with `jit.p` and `jit.v` that its building and arranging are compiled, not interpreted.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **fs.** Watching folders on macOS reads each folder again every half second. FSEvents would tell changes as they happen.
- **The core's libraries.** The standard plugins still use the core's SDL, SDL_ttf and stb_ds (DESIGN 17.2), against the principle that plugins stand alone (DESIGN 0.6). To remove: each plugin would bring what it uses.

## Shuild

[Shuild](https://github.com/omerfuyar/shuild) is the owner's; changes go to it as CLAUDE.md says.

## Reaching users

What helps people find OpenECS and stay. Before announcing a release widely:

- **GIFs of what OpenECS can do** in README.md, which the owner records.
- **GitHub settings**, which the owner sets for each of OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template:
  - a description, one line on what the repository holds;
  - topics, such as `sdl3`, `lua`, `editor`, `plugin-system`, `c23` and `tooling`;
  - a social preview image, such as a screenshot of a tool with its panels;
  - issues labelled "good first issue".

Then announce in this order, each post written for its community: SDL's forum and the Handmade Network; r/C_Programming, r/lua, r/gamedev, r/linux and r/opensource; Show HN and Lobsters; the Lua mailing list. After a release, a short devlog, and a monthly "how I built X with OpenECS" post.

What would grow it most:

- **Releases for macOS**, since most tool builders are not on Linux. Windows has one since 0.4.0 (see Tasks).
- **A flagship tool** that is useful without writing code, such as a log viewer, a glTF viewer or a terminal, built from the standard plugins.
  - **An editor for LÖVE games**, the owner's flagship: [ONLR](https://github.com/omerfuyar/ONLR), in its own repository, whose TODO.md holds its open questions. Its files panel, viewer and console are standard plugins now.
- **A showcase** in README.md of plugins and tools others build.

## Review

Recommendations from a review of the code, by area, the most important first. P1 is a bug, or a design flaw that gets harder to fix with each plugin; P2 is worth doing before 1.0.0; P3 when the code is touched. Each was checked at the lines it names; none was proven by a run. Paths under `std/` are OpenECS-std's.

**Core: communication**

- **P3 · No example frees a handle while Lua holds it.** Nothing in OpenECS-examples calls `ECSHandle_Free`, `ECSHandle_Forget` or `ECSValue_SetHandle`, or disables a plugin while another keeps its callback. Add cases when those paths change. Size: M.

**std: ui, with draw**

- **P3 · A long table or tree is built whole on each show.** `std/plugins/ui/init.lua` `elements.table`, `elements.tree` A list builds only the items in view (std DESIGN 5), but a table and a tree build every row: they are lists inside, so they could take `count` and `first` the same way, with `ui.range`. Profiler's table is short, and files' tree shows the open folders only; do it when a tool shows a long one. Size: M.

**Core: plugins and threads**

- **P3 · No example disables a plugin that is working.** Add a test that disables c13_workers' plugin while a worker counts, and c12_sketch's during an export, when those paths change. Size: M.

**Core: interface**

- **P3 · Window.c holds five modules.** `src/interface/Window.c:139-211` Its 2948 lines, against 2303 for the next largest module, hold the OS windows, and also the drag of panels and drop zones (`:458-730`, `:2384-2590`), the Clay declarations (`:732-1304`), the popups' views (`:1623-1848`), the title bar and hit tests, and the menus' geometry (`:2631-2765`). Move each to its own file with its part of the `WINDOW` struct; Window.c keeps the OS windows, renderers and frame pacing. The parts call each other, so three modules in an order avoid a cycle (DESIGN 1.5): Drag, then View with every Clay declaration, then Window. The owner leaves it until a change needs it: it causes no bug, and splitting it risks the drag and the menus. Size: L.
