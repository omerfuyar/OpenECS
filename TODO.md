# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Bugs** that the owner reports from testing.

## Tasks

- **SDL's tray closes the D-Bus connection of notifications.** On Linux, SDL's D-Bus tray calls `SDL_DBus_Quit` when its last tray goes, which closes the connection that SDL's notifications share: their clicks stop coming after notify's last tray goes. At exit, `SDL_CleanupNotifications` then removes a filter from a new connection, which libdbus aborts on; the core makes that a warning for its quit (`ECSIApp_QuitSDL`), so the workaround can go once SDL is fixed. It is SDL's bug, still in its `main`, for the owner to report: SDL takes no contributions written by AI.
- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now, and aarch64 waits for 1.0.0 (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags.
- **The lag on Windows.** The owner saw the editor lag on Windows in a Release build, on a fast machine (i7-14700, 64 GB) with low CPU and GPU use: on hovering, moving and resizing, and more in a larger window. It could not be reproduced on Linux, where software Vulkan hides it. Measure it with the profiler (Alt+W, then F; std DESIGN 32), and send its report; suspects are Direct3D 12's present and frame pacing, and `SDL_UpdateTexture` of large surfaces, which the steps `present` and `upload` show.
- **LuaJIT in place of Lua 5.5.** Not decided. LuaJIT is faster at running Lua, and its FFI calls C almost as C does, but only in code the JIT compiles, through `ffi.cdef` declarations; a function of the C API, such as `ECSIServices_CallC` and every `ecs` binding, is no faster there, and stops the JIT. The gain needs a new bridge: Lua would call a service's C function through the FFI, from a declaration made of its signature, and the bridge's work would move to Lua or go: its misuse checks, structs, handles, `out string error`, callbacks and the profiler's scopes (DESIGN 3.5, 10). Measured in a Release build on Linux, with settings, keys, files and console redrawn whole 50 times a second: their Lua code took about 330 ms a second, draw's C 160 ms, and the calls between them about 3% of the Lua time (0.15 µs a call into C, against 0.03 µs into Lua; about a thousand calls a frame). So the time is in the Lua code, mostly ui building and arranging its tree, which LuaJIT would run faster without a new bridge; the keys list's whole redraw takes about 5 ms. What changes:
  - **Numbers.** LuaJIT has only doubles. The core tells integers from numbers (`lua_isinteger`, the setting type `integer`, `i64` parameters), and the plugins do with `math.type` and `math.tointeger` (std's settings and ui); integers above 2^53 would need LuaJIT's boxed `int64_t`.
  - **Lua code.** Integer division `//` (ui, four examples), bitwise operators (ui's colours, c12_sketch, s6_web's SHA-1, s8_gpu, s10_cglm), `string.pack` and `string.unpack` (ui's triangles and pixels, five examples) and `utf8` (ui's text measure, c10_text_input, s1_terminal). LuaJIT has the `bit` library and the FFI for these, and no `utf8`.
  - **C code.** `lua_newuserdatauv`, the 5.4 `lua_resume`, `lua_setwarnf` and `luaL_makeseed` (`src/base/Lua.c`, `Services.c`, `Bindings.c`, `Workers.c`, `Test.c`): small changes.
  - **The build.** LuaJIT builds a host tool, `buildvm`, that writes its interpreter in assembly before the library compiles; shuild would build it as LuaJIT's makefile does, for each system and cross compiler. Lua builds from its C files alone.
  - **Upkeep.** LuaJIT follows Lua 5.1 with some of 5.2, and does not take later versions' features; its JIT exists for a fixed list of CPUs, and the platforms DESIGN 19.4 plans need checking against it. ecs.lua and every plugin would be written for 5.1, and LuaLS set to LuaJIT.

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

- **P3 · A column in a row measures its text at its parent's width.** `std/plugins/ui/init.lua` A row narrows its wrapped texts and columns only when its children need more than its width; a column that grows in a row still breaks its text at the width it had before it grew. Measure wrapped text again once widths are arranged. Size: M.
- **P3 · A long list or table is built whole each frame.** `std/plugins/ui/init.lua:536-545`, `:398-419` Each row is a node with two more for each cell, and a closure for each item with `onActivate`; all are measured and arranged, and only those in view are drawn. Rows have one height, so build and measure the rows in view, and share one handler among the items. A list takes children that are built already, so this needs a form of `ui.list` that takes a count and a function that builds one item; the owner leaves it until a tool needs it. Size: M.

**Core: plugins and threads**

- **P3 · No example disables a plugin that is working.** Add a test that disables c13_workers' plugin while a worker counts, and c12_sketch's during an export, when those paths change. Size: M.

**Core: interface**

- **P3 · Window.c holds five modules.** `src/interface/Window.c:139-211` Its 2848 lines hold the OS windows, and also the drag of panels and drop zones (`:458-730`, `:2384-2590`), the Clay declarations (`:732-1304`), the popups' views (`:1623-1848`), the title bar and hit tests, and the menus' geometry (`:2631-2765`). Move each to its own file with its part of the `WINDOW` struct; Window.c keeps the OS windows, renderers and frame pacing. The owner leaves it until it is necessary. The parts call each other, so three modules in an order avoid a cycle (DESIGN 1.5): Drag, then View with every Clay declaration, then Window. Size: L.
- **P3 · No example presses two buttons, or sends input between a layout change and a frame.** `src/app/Test.c:845` `test.press` and `test.release` exist and no example uses them. Add cases for the two findings above that the tests can reach. Size: S.
