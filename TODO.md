# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Bugs** that the owner reports from testing.

## Tasks

- **SDL's tray closes the D-Bus connection of notifications.** On Linux, SDL's D-Bus tray calls `SDL_DBus_Quit` when its last tray goes, which closes the connection that SDL's notifications share: their clicks stop coming after notify's last tray goes. At exit, `SDL_CleanupNotifications` then removes a filter from a new connection, which libdbus aborts on; the core makes that a warning for its quit (`ECSIApp_QuitSDL`), so the workaround can go once SDL is fixed. It is SDL's bug, still in its `main`, for the owner to report: SDL takes no contributions written by AI.
- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now, and aarch64 waits for 1.0.0 (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **fs.** Watching folders on macOS reads each folder again every half second. FSEvents would tell changes as they happen.

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

Recommendations from a review of the code, by area, the most important first. P1 is a bug, or a design flaw that gets harder to fix with each plugin; P2 is worth doing before 1.0.0; P3 when the code is touched. Each was checked at the lines it names. An item says when a run proved it or measured it: times are of a Release build, run offscreen, unless they say Debug, whose sanitizers slow C and allocations. Paths under `std/` are OpenECS-std's.

**Core: communication**

- **P3 · The FFI wrapper calls the core three times around each call.** `src/runtime/Services.c:3103` It calls `misuses()` twice and `profiling()` once through the FFI. Reading the misuse counter and the profile flag through pointers would cost less. Not measured apart. Size: S.
- **P3 · Event delivery frees its queue on each pass, and its order differs by kind.** `src/runtime/Events.c:415-419` The queue is freed and grown again each pass that had events; keep two arrays and swap them. A panel event posted during delivery joins the pass (`:401`), while a named event waits for the next (`:221-228`), so their order is not defined. Size: S.
- **P3 · Values allocate more than they need.** `src/base/Values.c:595` Each handle value copies its type's name, though handle types live until exit. Each table gets a string map with copied keys, even for one field (`:644-658`). `ECSILua_ToValue` (`src/base/Lua.c:197-229`) allocates a list of names for each nested table, then reads each field again. Size: M.
- **P3 · Lookups cost more as plugins grow.** `src/runtime/Services.c:2843-2864` `ECSIFfi_CallsLua` reads every function of every plugin for each wrapper, and `ECSIText_Add` (`:274-288`) copies the whole text on each add, which is quadratic for ui's definitions. Keep a flag per plugin, and grow the text with a capacity. Size: S.
- **P3 · No example frees a handle while Lua holds it.** Nothing in OpenECS-examples calls `ECSHandle_Free`, `ECSHandle_Forget` or `ECSValue_SetHandle`, or disables a plugin while another keeps its callback. Add cases when those paths change. Size: M.

**Core: plugins and threads**

- **P3 · A worker's message is copied twice.** `src/app/Workers.c:103-127` Encoding makes a Lua string that is copied into the message, and receiving copies it into another string before decoding (`:154-156`). One reused `string.buffer` per state copies once and decodes in place. The inbox also shifts with `arrdel(inbox, 0)` (`:326`). Size: S.
- **P3 · Each plugin's start walks its whole folder, even with reloading off.** `src/runtime/Plugins.c:704` `ECSIPlugin_NewestChange` reads every file's time (`:389-420`). Take it only when `ecs.reloadPlugins` is on. Size: S.
- **P3 · The profile makes its maps again each second.** `src/base/Profile.c:469` With the profile on, the plugins', panels' and functions' maps and their copied names are freed each second. Keep them and zero their counts. Size: S.
- **P3 · No example disables a plugin that is working.** Add a test that disables c13_workers' plugin while a worker counts, and c12_sketch's during an export, when those paths change. A worker's script that fails, a dialog from a worker's `message`, and a plugin whose start fails after it started work are untested too. Size: M.

**Core: interface**

- **P2 · Each frame of an OS window declares its whole interface again.** `src/interface/Window.c:2042-2046` A window draws when one of its panels wants a frame, so a panel that animates, or each stroke of a sketch, makes the title bar, tabs and hint again, and measures the title with `TTF_GetStringSize` (`:1163-1164`). Keep Clay's commands of the last frame while nothing of the interface changed. Not measured. Size: M.
- **P3 · The title bar's close mark changes the shared font's size each frame.** `src/interface/Window.c:894` It draws at 1.4 times the font size, and `TTF_SetFontSize` is called on the one `WINDOW.font` (`:251`, `:1163`, `:2763`). Give the large size its own copy of the font. Not measured. Size: S.
- **P3 · The list of drawn tabs is freed each frame.** `src/interface/Window.c:1295` `arrfree` then `arrput` (`:956`); `arrsetlen(tabs, 0)` keeps its room, as `ECSIWindow_Forget` does (`:481`). Size: S.
- **P3 · Hover focus reads its setting by name on each pointer move.** `src/interface/Input.c:847` Keep it in a flag that the setting's change updates, as Keys does for `ecs.prefix`. Size: S.
- **P3 · Finding a panel's root walks every tree, several times a frame.** `src/interface/Layout.c:1640` `ECSILayout_RootOf` goes through `ECSILayout_Locate` (`:226-249`) for the interface, each popup, and each pointer move while a button is held. Keep a group pointer on each panel. Size: M.
- **P3 · Window.c holds five modules.** `src/interface/Window.c:139-211` Its 2948 lines, against 2303 for the next largest module, hold the OS windows, and also the drag of panels and drop zones (`:458-730`, `:2384-2590`), the Clay declarations (`:732-1304`), the popups' views (`:1623-1848`), the title bar and hit tests, and the menus' geometry (`:2631-2765`). Move each to its own file with its part of the `WINDOW` struct; Window.c keeps the OS windows, renderers and frame pacing. The parts call each other, so three modules in an order avoid a cycle (DESIGN 1.5): Drag, then View with every Clay declaration, then Window. The owner leaves it until a change needs it: it causes no bug, and splitting it risks the drag and the menus. Size: L.

**std: ui, with draw**

- **P2 · A partial redraw is one rectangle around every change.** `std/plugins/ui/init.lua:363-391` `changedArea` cuts the equal calls at both ends and bounds all between, so a hover at the top and a tooltip at the bottom redraw most of the panel. When both shows made as many calls, bound only the pairs that differ, in a few rectangles, and replay each, as scrolling does (`:4596-4601`). Size: M.
- **P2 · Text of spans is laid out from nothing on each show.** `std/plugins/ui/init.lua:1005-1066` Wrapped text is kept (`wrapKept`), but `layoutSpans` runs at each measure, with `gmatch`, which stops traces, new line and run tables, and a style table and `table.concat` per word in `spanFont` (`:982-1001`). Keep span lines by font, size, width and the spans' texts and styles, as `wrapKept` does. It weighs most on markdown. Not measured. Size: M.
- **P2 · A C panel's elements are copied three or four times on each draw.** `std/plugins/ui/header.h:397` The value tree is made and freed each draw, converted to Lua tables, copied again by `build` (`init.lua:4814`), which also makes a closure per callback (`:4842`), and copied once more by `element`. Use the converted props as they are, and keep the callbacks by panel and id. Not measured. Size: M.
- **P3 · draw wraps the surface in an SDL surface and lays out the text again for each text.** `std/plugins/draw/Draw.c:898-917` Measured: `draw.text` takes 1.7 µs for "Remove" and 13.3 µs for 62 characters, while measuring those 62 takes 0.11 µs, so the glyphs' blending takes the time and a cache of `TTF_Text` would gain little. Keep one wrapped SDL surface per pixels surface, for text and images (`:660-686`). Size: S.
- **P3 · A long table or tree is built whole on each show.** `std/plugins/ui/init.lua` `elements.table`, `elements.tree` A list builds only the items in view (std DESIGN 5), but a table and a tree build every row: they are lists inside, so they could take `count` and `first` the same way, with `ui.range`. Profiler's table is short, and files' tree shows the open folders only; do it when a tool shows a long one. Size: M.
- **P3 · init.lua is one file of 5263 lines.** `std/plugins/ui/init.lua` Three parts stand alone: the recording, comparing and replaying of draw calls (`:180-750`), the popups of dropdowns, menus and the colour picker (`:2843-3468`), and descriptions from C (`:4773-4923`). A plugin may `require` its own modules (DESIGN 9.6); keep layout, paint and events together, as they share state. Size: M.

**std: other plugins**

- **P3 · textgrid moves the whole grid for each line it scrolls.** `std/plugins/textgrid/TextGrid.c:166` Each line past the bottom moves every other row: 190 KB on a grid of 200 by 60. Keep the rows in a ring with a first row. Not measured. Size: M.
- **P3 · textgrid draws the cursor past the right edge.** `std/plugins/textgrid/TextGrid.c:603` After a row is filled to its end, the cursor's column is the grid's width, which `TextGridDraw` draws one cell outside (`:706`) and `cursor()` gives, though `setCursor` would not take it (`:511`). Draw it at the last column. Size: S.
- **P3 · parse and net copy and allocate on each call.** `std/plugins/parse/Text.c:683` CSV builds each field a byte at a time, then copies it twice (`Parse.c:163`); each JSON string is copied (`Json.c:53`) and writes copy their output twice (`Json.c:225-227`, `Parse.c:55`); net allocates its most on each `receive` (`std/plugins/net/Net.c:592`) and copies each datagram (`:924`). Keep one scratch array per plugin and a receive buffer per connection. Size: S.
- **P3 · console filters every entry on each draw, and shifts them all on each add.** `std/plugins/console/init.lua:259-265` Each draw matches all 2000 entries and makes new lists; each `add` past the limit runs `table.remove(entries, 1)` (`:161`). Keep the filtered list until an entry, the search or a toggle changes, and the entries in a ring. Size: S.
- **P3 · profiler formats its table again on each draw.** `std/plugins/profiler/init.lua:351` Its rows and graph change every half second, in `read()`, but each draw makes them again (`:93-115`, `:139-158`). Make them in `read()`. Size: S.
- **P3 · web's queues shift on each message.** `std/plugins/web/Web.c:799` `arrdel(inbox, 0)` and `arrdel(outbox, 0)` (`:187`) move the whole queue; read from a head. Size: S.
- **P3 · Their examples miss the failure paths.** s13_process has no `write`, `closeInput` or `kill`; s12_compress no `zip.extract`, its refusal of `..` and absolute names, or zstd; s11_parse no TOML, INI or .env reads of malformed text; s3_network no peer that closes or untrusted certificate; s1_terminal no scrolling past the last row or `put`; and no example calls fs, so its `overflow` and `gone` events, `copy` and `makeFolder` are untested. Size: M.

**Tools**

- **P2 · The core's own handles use a second object system, which the JIT cannot compile.** `src/app/Bindings.c:2183` A plugin's handle is an FFI cdata made with `ffi.metatype` (`src/runtime/Services.c:1698`), but the core's panel, surface, timer and subscription are userdata whose `__index` is a C function that compares the key's text (`Bindings.c:1704`, `:2183-2215`). Measured: reading `surface.width` takes 93 ns, and `surface:fill` 216 ns, where `draw.fill` takes 87. Make the core's handles the same FFI handles as plugins', and call through FFI wrappers the core functions that never run Lua, such as a surface's `fill`, `setPixel` and `setRow`. Lua code reads the same; one handle system is left. Size: L.
