# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Bugs** that the owner reports from testing.
- **Editors that are simpler to build.** What each panel of an editor writes again today, and what an editor needs and writes itself: the panel type's `redrawOn`, keys for all of a plugin's panels, and the pointer's moves while data is dragged (Tasks); `ui.panelType` and the plugin history (Standard plugins).

## Tasks

- **SDL's tray closes the D-Bus connection of notifications.** On Linux, SDL's D-Bus tray calls `SDL_DBus_Quit` when its last tray goes, which closes the connection that SDL's notifications share: their clicks stop coming after notify's last tray goes. At exit, `SDL_CleanupNotifications` then removes a filter from a new connection, which libdbus aborts on; the core makes that a warning for its quit (`ECSIApp_QuitSDL`), so the workaround can go once SDL is fixed. It is SDL's bug, still in its `main`, for the owner to report: SDL takes no contributions written by AI.
- **The workspaces' list chooses the first workspace.** The title bar's workspaces' menu and `ecs.workspace.menu` open with the first entry chosen. They should choose the current workspace, so Down goes to the next one; c12_sketch's `test-workspaces.lua` then presses Down once for each step.
- **A menu's keys can reach its right edge.** In the panel menu of a clock in c12_sketch, "Alt+W, M" touches the menu's edge. The menu should be as wide as its widest label and keys, with its padding.
- **A panel type redraws on events.** Plugins keep a table of their open panels only to redraw them all when one of their events comes, such as std's files, the template's notes and c5_services' tally. A field `redrawOn` of the panel type, in `ECSPanelTypeDesc` and `ecs.PanelTypeDesc`, names events; when one is emitted, every panel of the type is drawn again. The plugins that keep such tables then drop them, in the same piece of work.
- **Keys for all of a plugin's panels.** `ecs.key.bind` takes one panel type, or `"prefix"`. A tool whose undo, redo and save work in each of its panels binds each key once for each type. A scope of the plugin's own name binds a key in every panel type of the plugin.
- **While data is dragged, no panel gets the pointer's moves.** DESIGN 7.9 says only the panel that started the drag gets none, but `ECSIInput_MovePointer` (`src/interface/Input.c:665`) gives them to no panel. So a list or a tree that accepts the data cannot show the line where it will land before the release; ui's `onDrop` tells it only then. Give the moves to the panel under the pointer, as 7.9 says.
- **A test reads the open menu.** Tests choose menu entries by counting Down and Up, so an entry that is added or hidden breaks them. `test.menu()` would give the open menu's entries: their labels, keys, whether they can be used, and which is chosen.
- **A test changes a setting.** `test.setting` only reads one. A test of another scale needs a preset of its own, such as c3_ui's `preset_large.lua` and c12_sketch's `preset-large.lua`, and a change while the tool runs cannot be tested. `test.setSetting(name, value)` would change it as the user's settings file does.
- **A test calls a function with values, and answers a file dialog.** `test.call` runs a function that takes nothing, and nothing answers the system's file dialogs in a test (DESIGN 7.7), so a tool's New and Open cannot be tested. `test.call(name, ...)` would pass the values, and `test.fileDialog()` would read the waiting dialog's description and answer it with paths.
- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now, and aarch64 waits for 1.0.0 (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **fs.** Watching folders on macOS reads each folder again every half second. FSEvents would tell changes as they happen.
- **fs: more of what a files panel needs.** Watching a folder with the folders inside it; moving a file to the system's trash, for the files panel's `delete`; and a file's permissions, such as whether it runs as a program.
- **history: undo and redo for any tool.** A standard plugin, as every editor needs it. A plugin pushes records, each a label with an undo and a redo function. history gives undo and redo as functions, merges quick changes of one value into one record, and keeps a save point that sets the unsaved mark of the panels it names. A History panel lists the records, and a click on one undoes or redoes until it is the last one done. Open: one history for the tool, or one for each document a tool has open.
- **ui.panelType.** Each panel type built with ui gives `event = ui.event`, calls `ui.forget` in `destroy` and `ui.show` in `draw`. `ui.panelType(desc)` would register a panel type whose `view(state)` gives the element tree, and do the rest. Std's panels, the examples and the template then use it, in the same piece of work.
- **ui: a double click on an item.** `tree` and `list` give a click to `onActivate`, so the files panel (29) and the console (31) each time a second click themselves, within 0.4 seconds. They should give a double click to its own handler.

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
- **P3 · No example forgets a handle, or disables a plugin while another keeps its callback.** c12_sketch frees a handle while Lua holds it, with `ECSHandle_Free`, but nothing in OpenECS-examples calls `ECSHandle_Forget` or `ECSValue_SetHandle`, or disables a plugin while another keeps its callback. Add cases when those paths change. Size: M.

**Core: plugins and threads**

- **P3 · A worker's message is copied twice.** `src/app/Workers.c:103-127` Encoding makes a Lua string that is copied into the message, and receiving copies it into another string before decoding (`:154-156`). One reused `string.buffer` per state copies once and decodes in place. The inbox also shifts with `arrdel(inbox, 0)` (`:326`). Size: S.
- **P3 · Each plugin's start walks its whole folder, even with reloading off.** `src/runtime/Plugins.c:704` `ECSIPlugin_NewestChange` reads every file's time (`:389-420`). Take it only when `ecs.reloadPlugins` is on. Size: S.
- **P3 · The profile makes its maps again each second.** `src/base/Profile.c:469` With the profile on, the plugins', panels' and functions' maps and their copied names are freed each second. Keep them and zero their counts. Size: S.
- **P3 · No example disables a plugin during its background work.** c13_workers disables its plugin while its workers count, but c12_sketch's export, which runs with `ECS_RunInBackground`, starts from a file dialog that a test cannot answer, so no test disables sketch_c during one. A worker's script that fails, a dialog from a worker's `message`, and a plugin whose start fails after it started work are untested too. Size: M.

**Core: interface**

- **P3 · Each frame of an OS window declares its whole interface again.** `src/interface/Window.c:2042-2046` A panel that animates makes the title bar, tabs and hint again on each frame. Measured, with a panel that draws on each frame beside seven still ones: declaring the interface takes 21.8 µs of a frame of 2662 µs, and composing the frame in the software renderer 1434 µs; the frames of three runs spread over 25 µs. Keeping Clay's commands would gain at most the 21.8 µs, within that spread, so it was not done. Size: M.
- **P3 · The title bar's close mark changes the shared font's size each frame.** `src/interface/Window.c:894` It draws at 1.4 times the font size, and `TTF_SetFontSize` is called on the one `WINDOW.font` (`:251`, `:1163`, `:2763`). Give the large size its own copy of the font. Not measured. Size: S.
- **P3 · The list of drawn tabs is freed each frame.** `src/interface/Window.c:1295` `arrfree` then `arrput` (`:956`); `arrsetlen(tabs, 0)` keeps its room, as `ECSIWindow_Forget` does (`:481`). Size: S.
- **P3 · Hover focus reads its setting by name on each pointer move.** `src/interface/Input.c:847` Keep it in a flag that the setting's change updates, as Keys does for `ecs.prefix`. Size: S.
- **P3 · Finding a panel's root walks every tree, several times a frame.** `src/interface/Layout.c:1640` `ECSILayout_RootOf` goes through `ECSILayout_Locate` (`:226-249`) for the interface, each popup, and each pointer move while a button is held. Keep a group pointer on each panel. Size: M.
- **P3 · Window.c holds five modules.** `src/interface/Window.c:139-211` Its 2948 lines, against 2303 for the next largest module, hold the OS windows, and also the drag of panels and drop zones (`:458-730`, `:2384-2590`), the Clay declarations (`:732-1304`), the popups' views (`:1623-1848`), the title bar and hit tests, and the menus' geometry (`:2631-2765`). Move each to its own file with its part of the `WINDOW` struct; Window.c keeps the OS windows, renderers and frame pacing. The parts call each other, so three modules in an order avoid a cycle (DESIGN 1.5): Drag, then View with every Clay declaration, then Window. The owner leaves it until a change needs it: it causes no bug, and splitting it risks the drag and the menus. Size: L.

**std: ui, with draw**

- **P3 · A C panel's elements are copied three or four times on each draw.** `std/plugins/ui/header.h:397` The value tree is made and freed each draw, converted to Lua tables, copied again by `build` (`init.lua`), and once more by `element`. Measured with c3_ui's panels, a hover each draw: the C panel's draw takes 493.4 µs and the Lua twin's 400.8 µs, and `build` 77 µs of the difference, or 28 µs when it runs in a loop. Keeping the callbacks by panel and id, and giving `element` the description's props as they are where nothing changes them, took it to 493.6 µs: no gain, within the runs' spread of 43 µs, so it was not done. A Lua panel may keep its description and show it again, as s19_markdown does, so `build` cannot change the given props in place. Size: M.
- **P3 · draw wraps the surface in an SDL surface and lays out the text again for each text.** `std/plugins/draw/Draw.c:898-917` Measured: `draw.text` takes 1.7 µs for "Remove" and 13.3 µs for 62 characters, while measuring those 62 takes 0.11 µs, so the glyphs' blending takes the time and a cache of `TTF_Text` would gain little. Keep one wrapped SDL surface per pixels surface, for text and images (`:660-686`). Size: S.
- **P3 · A long table is built whole on each show.** `std/plugins/ui/init.lua` `elements.table` A list and a tree build only the items in view (std DESIGN 5), but a table builds every row: it is a list inside, so it could take `count` and `first` the same way, with `ui.range`. Profiler's table is short; do it when a tool shows a long one. Size: S.
- **P3 · init.lua is one file of 5263 lines.** `std/plugins/ui/init.lua` Three parts stand alone: the recording, comparing and replaying of draw calls (`:180-750`), the popups of dropdowns, menus and the colour picker (`:2843-3468`), and descriptions from C (`:4773-4923`). A plugin may `require` its own modules (DESIGN 9.6); keep layout, paint and events together, as they share state. Size: M.

**std: other plugins**

- **P3 · textgrid moves the whole grid for each line it scrolls.** `std/plugins/textgrid/TextGrid.c:166` Each line past the bottom moves every other row: 190 KB on a grid of 200 by 60. Keep the rows in a ring with a first row. Not measured. Size: M.
- **P3 · parse and net copy and allocate on each call.** `std/plugins/parse/Text.c:683` CSV builds each field a byte at a time, then copies it twice (`Parse.c:163`); each JSON string is copied (`Json.c:53`) and writes copy their output twice (`Json.c:225-227`, `Parse.c:55`); net allocates its most on each `receive` (`std/plugins/net/Net.c:592`) and copies each datagram (`:924`). Keep one scratch array per plugin and a receive buffer per connection. Size: S.
- **P3 · console shifts all its entries on each add past the limit.** `std/plugins/console/init.lua` `add` runs `table.remove(entries, 1)` once the limit is reached. Keep the entries in a ring. Size: S.
- **P3 · profiler formats its table again on each draw.** `std/plugins/profiler/init.lua:351` Its rows and graph change every half second, in `read()`, but each draw makes them again (`:93-115`, `:139-158`). Make them in `read()`. Size: S.
- **P3 · web's queues shift on each message.** `std/plugins/web/Web.c:799` `arrdel(inbox, 0)` and `arrdel(outbox, 0)` (`:187`) move the whole queue; read from a head. Size: S.
- **P3 · Their examples miss failure paths.** s3_network has no peer that closes or untrusted certificate; s1_terminal no scrolling past the last row or `put`; and no example calls fs, so its `overflow` and `gone` events, `copy` and `makeFolder` are untested. Size: M.
