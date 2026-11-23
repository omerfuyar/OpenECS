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

- **P2 · A C function called through the FFI can still run Lua through the core.** `src/runtime/Services.c:2869` `ECSIFfi_Wraps` gives a plugin's C function an FFI wrapper unless the plugin keeps callbacks or looked up a Lua function. Its C code can still call a core function that runs Lua, such as `ECSLayout_Open` of a Lua panel type, whose `create` runs through `ECSILua_Call` (`src/base/Lua.c:938`), which does not check `ECSILua_InFfiCall` as `RunLua` does (`Services.c:2267`). The choice is also kept from the first lookup (`:4615-4629`), so a plugin that looks up a Lua function later keeps its wrappers. Make Lua run by the core inside an FFI call a misuse, as DESIGN 10.4 makes a Lua function's: check in `ECSILua_Call`. No shipped C function does this yet; sketch_c opens its own C panels. Size: M.
- **P2 · Handle values outside queued events keep pointers to freed objects.** `src/runtime/Events.c:331` `ECSIValue_ForgetObject` runs only on queued emissions. The drag's value (`src/interface/Input.c:1232`) and drop data (`src/interface/Panels.c:838`) keep the raw object, and `PushHandleOf` (`Services.c:4305-4326`) then makes a live handle for freed or reused memory. Forget objects there too. Size: S.
- **P3 · The FFI wrapper calls the core three times around each call.** `src/runtime/Services.c:3103` It calls `misuses()` twice and `profiling()` once through the FFI. Reading the misuse counter and the profile flag through pointers would cost less. Not measured apart. Size: S.
- **P3 · Each callback call makes tables.** `src/runtime/Services.c:2290-2293` `RunLua` makes its `anchors` and `callbacks` tables on each call, a spare callback makes a new anchors table (`:1500`), and `RenewResult` (`:2243`) makes the result value again. This hits `draw.clip` on each ui show. Note in the signature whether it has callbacks or byte and handle outputs, and make the tables only then. Size: S.
- **P3 · Event delivery frees its queue on each pass, and its order differs by kind.** `src/runtime/Events.c:415-419` The queue is freed and grown again each pass that had events; keep two arrays and swap them. A panel event posted during delivery joins the pass (`:401`), while a named event waits for the next (`:221-228`), so their order is not defined. Size: S.
- **P3 · Values allocate more than they need.** `src/base/Values.c:595` Each handle value copies its type's name, though handle types live until exit. Each table gets a string map with copied keys, even for one field (`:644-658`). `ECSILua_ToValue` (`src/base/Lua.c:197-229`) allocates a list of names for each nested table, then reads each field again. Size: M.
- **P3 · Lookups cost more as plugins grow.** `src/runtime/Services.c:2843-2864` `ECSIFfi_CallsLua` reads every function of every plugin for each wrapper, and `ECSIText_Add` (`:274-288`) copies the whole text on each add, which is quadratic for ui's definitions. Keep a flag per plugin, and grow the text with a capacity. Size: S.
- **P3 · The header, ecs.lua and DESIGN disagree.** `include/OpenECS.h:789` says a Lua handle is a userdata, where DESIGN 10.6 says a cdata. `OpenECS.h:814` leaves `%`, `^` and `..` out of the operators that `ecs.lua:732` and DESIGN 10.11 list. DESIGN 8.3 declares `"canvas.selectionChanged"` with its plugin's name, which `Plugins.c:1169` refuses. `ecs.lua:671` makes an event's `description` optional, which `Events.c:350` requires. Size: S.
- **P3 · No example frees a handle while Lua holds it.** Nothing in OpenECS-examples calls `ECSHandle_Free`, `ECSHandle_Forget` or `ECSValue_SetHandle`, or disables a plugin while another keeps its callback. Add cases when those paths change. Size: M.

**Core: plugins and threads**

- **P2 · A plugin whose start fails keeps its workers and background work.** `src/runtime/Plugins.c:529` `ECSIPlugin_Fail` removes registrations (`src/app/App.c:131-139`) but skips the stops that disabling runs (`App.c:782-785`). A native plugin enabled while the program runs is unloaded (`Plugins.c:541`) while its work may still run on a pool thread, and a Lua plugin's worker goes on calling it. Stop a plugin's code in one function, used by both. Size: S.
- **P3 · Each protected call allocates a function.** `src/base/Lua.c:944` LuaJIT has no light C functions, so `lua_pushcfunction(state, ECSILua_Traceback)` makes a new function at each `ECSILua_Call`: each callback, draw and event. So do `Workers.c:338` and `:506`. Make it once per state and keep it in the registry. Size: S.
- **P3 · A worker's message is copied twice.** `src/app/Workers.c:103-127` Encoding makes a Lua string that is copied into the message, and receiving copies it into another string before decoding (`:154-156`). One reused `string.buffer` per state copies once and decodes in place. The inbox also shifts with `arrdel(inbox, 0)` (`:326`). Size: S.
- **P3 · Thread-local errors leak on worker threads.** `src/runtime/Services.c:4231` `SERVICES_THREAD.error` and `PLUGINS_THREAD.misuse` (`src/runtime/Plugins.c:1700`) are copied and never freed when a worker or pool thread ends. Free them at the end of `ECSIWorkers_Run` and `ECSITasks_Worker`. Size: S.
- **P3 · Each plugin's start walks its whole folder, even with reloading off.** `src/runtime/Plugins.c:704` `ECSIPlugin_NewestChange` reads every file's time (`:389-420`). Take it only when `ecs.reloadPlugins` is on. Size: S.
- **P3 · The profile makes its maps again each second.** `src/base/Profile.c:469` With the profile on, the plugins', panels' and functions' maps and their copied names are freed each second. Keep them and zero their counts. Size: S.
- **P3 · Flags are read across threads without a lock or an atomic.** `src/runtime/Plugins.c:1267` `stopping`, `failed` and `disabled` are read under `TASKS.lock` and written without it (`:1261`, `:1422`, `:531`), and `PROFILE.on` is read on worker threads (`src/base/Profile.c:364`). They are ordered by the locks that follow today; make them atomics so a thread sanitizer runs clean. Size: S.
- **P3 · No example disables a plugin that is working.** Add a test that disables c13_workers' plugin while a worker counts, and c12_sketch's during an export, when those paths change. A worker's script that fails, a dialog from a worker's `message`, and a plugin whose start fails after it started work are untested too. Size: M.

**Core: interface**

- **P2 · A tab's title and unsaved mark show late.** `src/interface/Panels.c:1395-1407` `ECSPanel_SetTitle` asks for no frame. `ECSPanel_SetUnsaved` (`:1291-1298`) sets `needsDraw`, which `Layout.c:1159-1166` reads only for each group's shown panel, so a tab behind another shows its mark only when something else draws; on the shown panel it costs a whole draw and upload. Ask for a frame of the interface when either changes. Size: S.
- **P2 · Pointers to panels and popups kept between passes can match new ones.** `src/interface/Input.c:178-192` The pressed panel and popup are checked by comparing pointers, and so are `textPanel` (`:1193`) and the Tabs menu's panels (`src/interface/Menus.c:227`, `:268`). A panel made after one closed can get its address, and then gets its input. Keep panel ids, or clear these fields when a panel or popup closes. Size: S.
- **P2 · Each frame of an OS window declares its whole interface again.** `src/interface/Window.c:2042-2046` A window draws when one of its panels wants a frame, so a panel that animates, or each stroke of a sketch, makes the title bar, tabs and hint again, and measures the title with `TTF_GetStringSize` (`:1163-1164`). Keep Clay's commands of the last frame while nothing of the interface changed. Not measured. Size: M.
- **P3 · The title bar's close mark changes the shared font's size each frame.** `src/interface/Window.c:894` It draws at 1.4 times the font size, and `TTF_SetFontSize` is called on the one `WINDOW.font` (`:251`, `:1163`, `:2763`). Give the large size its own copy of the font. Not measured. Size: S.
- **P3 · The list of drawn tabs is freed each frame.** `src/interface/Window.c:1295` `arrfree` then `arrput` (`:956`); `arrsetlen(tabs, 0)` keeps its room, as `ECSIWindow_Forget` does (`:481`). Size: S.
- **P3 · Hover focus reads its setting by name on each pointer move.** `src/interface/Input.c:847` Keep it in a flag that the setting's change updates, as Keys does for `ecs.prefix`. Size: S.
- **P3 · Finding a panel's root walks every tree, several times a frame.** `src/interface/Layout.c:1640` `ECSILayout_RootOf` goes through `ECSILayout_Locate` (`:226-249`) for the interface, each popup, and each pointer move while a button is held. Keep a group pointer on each panel. Size: M.
- **P3 · Window.c holds five modules.** `src/interface/Window.c:139-211` Its 2948 lines, against 2303 for the next largest module, hold the OS windows, and also the drag of panels and drop zones (`:458-730`, `:2384-2590`), the Clay declarations (`:732-1304`), the popups' views (`:1623-1848`), the title bar and hit tests, and the menus' geometry (`:2631-2765`). Move each to its own file with its part of the `WINDOW` struct; Window.c keeps the OS windows, renderers and frame pacing. The parts call each other, so three modules in an order avoid a cycle (DESIGN 1.5): Drag, then View with every Clay declaration, then Window. The owner leaves it until a change needs it: it causes no bug, and splitting it risks the drag and the menus. Size: L.

**std: ui, with draw**

- **P2 · Rows measure their children again, and each measure drops its layout table.** `std/plugins/ui/init.lua:2020` A row measures a child again at its new width, which measures its whole subtree again, so nested rows and columns measure 2^depth times. Each `measure` takes a new layout table from the spares (`:2036-2040`) and drops the last. Measured in Debug: 9% of the samples of a hover over the key list are the garbage collector, half of them in `measureRowWidths`. Reuse the layout table the same show gave, and measure a child again only when its width changes. Size: M.
- **P2 · ui's own painting makes closures, tables and FFI arrays on each show, which stop LuaJIT's traces.** `std/plugins/ui/init.lua:2458` Each field, scroll element and the show itself pass `draw.clip` a new closure (`:2458`, `:2540`, `:4576`), each clip makes a table and a key text (`:296-312`), and `arrow` makes a `float[?]` from varargs (`:13-15`, `:2351`). jit.v shows the traces through them abort at `FNEW`, `UCLO` and "unsupported C type conversion", and in Debug 17% of a hover's samples run interpreted. Let `draw.clip` pass the arguments after `body` to it, so ui passes its own functions with their arguments, keep clip records with the reused calls, and fill one `float[6]` for arrows. The change to `draw.clip` changes draw's interface and keeps its guarantee (std DESIGN 2). A view's own closures cost little: taking the key list's three closures per row out changed 400 hovers from 1178 to 1177 ms (Debug). Size: M.
- **P2 · A partial redraw is one rectangle around every change.** `std/plugins/ui/init.lua:363-391` `changedArea` cuts the equal calls at both ends and bounds all between, so a hover at the top and a tooltip at the bottom redraw most of the panel. When both shows made as many calls, bound only the pairs that differ, in a few rectangles, and replay each, as scrolling does (`:4596-4601`). Size: M.
- **P2 · Text of spans is laid out from nothing on each show.** `std/plugins/ui/init.lua:1005-1066` Wrapped text is kept (`wrapKept`), but `layoutSpans` runs at each measure, with `gmatch`, which stops traces, new line and run tables, and a style table and `table.concat` per word in `spanFont` (`:982-1001`). Keep span lines by font, size, width and the spans' texts and styles, as `wrapKept` does. It weighs most on markdown. Not measured. Size: M.
- **P2 · A C panel's elements are copied three or four times on each draw.** `std/plugins/ui/header.h:397` The value tree is made and freed each draw, converted to Lua tables, copied again by `build` (`init.lua:4814`), which also makes a closure per callback (`:4842`), and copied once more by `element`. Use the converted props as they are, and keep the callbacks by panel and id. Not measured. Size: M.
- **P2 · The colour picker does nothing from the keys.** `std/plugins/ui/init.lua:3437-3451` A colour element takes Tab and opens with Return (`:3500`), but its picker hears only the pointer and Return, unlike the dropdown (`:3051-3067`) and the menu (`:3248-3255`). Let the arrows move saturation and value, and Shift with them hue and alpha, and add a key test to c3_ui. Size: M.
- **P2 · C cannot use all that Lua's elements offer.** `std/plugins/ui/header.h:49-90` `UiProps` has no `folders`, so a C dropdown shows no folders (`types.lua:197`), and its `selected` is one `i32`, so a C list, table or tree cannot choose several items (`types.lua:75`, `:108`, `:136`). Add `folders`, and a list of chosen items or a line in std DESIGN 5 that C chooses one. Size: M.
- **P2 · Translucent fills cost 19 times opaque ones, and rounded shapes 11 times more.** `std/plugins/draw/Draw.c:84-104` `DrawBlend` multiplies and divides each channel of each pixel. `DrawSmooth` (`:123-136`) computes a distance and a rounding for every pixel of a rounded rectangle's, circle's or line's box, inside and outline's hole included. Measured on 1280 by 770 pixels: an opaque fill 0.07 ns a pixel, one with alpha 1.36, a rounded one with alpha 15. Blend two channels at once with the `0x00FF00FF` mask, fill the inside of rounded shapes as spans, and compute coverage only at their edges. Size: M.
- **P3 · Each tree folder's arrow is a stop of Tab.** `std/plugins/ui/init.lua:1349` The arrow and each mark are rows with `onClick` (`:1367-1378`), so crossing a tree of 200 folders takes 200 Tabs, where Left and Right already close and open the chosen one (`:3805`). Leave them out of the Tab order. Size: S.
- **P3 · `assignIds` makes a path text for each element on each show.** `std/plugins/ui/init.lua:4454-4475` It joins `path .. "." .. place` even for an element that has an id, and its loop aborts traces at the unroll limit (jit.v). Make the path only for elements without an id. Size: S.
- **P3 · draw wraps the surface in an SDL surface and lays out the text again for each text.** `std/plugins/draw/Draw.c:898-917` Measured: `draw.text` takes 1.7 µs for "Remove" and 13.3 µs for 62 characters, while measuring those 62 takes 0.11 µs, so the glyphs' blending takes the time and a cache of `TTF_Text` would gain little. Keep one wrapped SDL surface per pixels surface, for text and images (`:660-686`). Size: S.
- **P3 · A long table or tree is built whole on each show.** `std/plugins/ui/init.lua` `elements.table`, `elements.tree` A list builds only the items in view (std DESIGN 5), but a table and a tree build every row: they are lists inside, so they could take `count` and `first` the same way, with `ui.range`. Profiler's table is short, and files' tree shows the open folders only; do it when a tool shows a long one. Size: M.
- **P3 · init.lua is one file of 5263 lines.** `std/plugins/ui/init.lua` Three parts stand alone: the recording, comparing and replaying of draw calls (`:180-750`), the popups of dropdowns, menus and the colour picker (`:2843-3468`), and descriptions from C (`:4773-4923`). A plugin may `require` its own modules (DESIGN 9.6); keep layout, paint and events together, as they share state. Size: M.

**std: other plugins**

- **P2 · A size read from an archive or a reply sizes an stb_ds array.** `std/plugins/compress/Compress.c:458` `arrsetlen` takes the zip entry's own `uncompressed_size`, and stb_ds aborts the program when it cannot allocate, so a damaged archive ends the program instead of failing. So do inflate and zstd's growth (`:190`, `:285`) and web's bodies (`std/plugins/web/Web.c:131`). Allocate with `SDL_malloc` and fail, or grow while reading; write the rule in std DESIGN 1.3. Size: S.
- **P2 · parse does not follow std's error rules.** `std/plugins/parse/Parse.c:38-50` `ParseFail` never calls `ECS_Fail`, so its failures skip `ecs.logServiceErrors`, and a separator of more than one character, a misuse, is given as an error (`std/plugins/parse/Text.c:617`). Call `ECS_Fail` as compress's `CompressFail` does, and `ECS_Check` the separator. Size: S.
- **P3 · textgrid moves the whole grid for each line it scrolls.** `std/plugins/textgrid/TextGrid.c:166` Each line past the bottom moves every other row: 190 KB on a grid of 200 by 60. Keep the rows in a ring with a first row. Not measured. Size: M.
- **P3 · textgrid draws the cursor past the right edge.** `std/plugins/textgrid/TextGrid.c:603` After a row is filled to its end, the cursor's column is the grid's width, which `TextGridDraw` draws one cell outside (`:706`) and `cursor()` gives, though `setCursor` would not take it (`:511`). Draw it at the last column. Size: S.
- **P3 · parse and net copy and allocate on each call.** `std/plugins/parse/Text.c:683` CSV builds each field a byte at a time, then copies it twice (`Parse.c:163`); each JSON string is copied (`Json.c:53`) and writes copy their output twice (`Json.c:225-227`, `Parse.c:55`); net allocates its most on each `receive` (`std/plugins/net/Net.c:592`) and copies each datagram (`:924`). Keep one scratch array per plugin and a receive buffer per connection. Size: S.
- **P3 · console filters every entry on each draw, and shifts them all on each add.** `std/plugins/console/init.lua:259-265` Each draw matches all 2000 entries and makes new lists; each `add` past the limit runs `table.remove(entries, 1)` (`:161`). Keep the filtered list until an entry, the search or a toggle changes, and the entries in a ring. Size: S.
- **P3 · profiler formats its table again on each draw.** `std/plugins/profiler/init.lua:351` Its rows and graph change every half second, in `read()`, but each draw makes them again (`:93-115`, `:139-158`). Make them in `read()`. Size: S.
- **P3 · web's queues shift on each message.** `std/plugins/web/Web.c:799` `arrdel(inbox, 0)` and `arrdel(outbox, 0)` (`:187`) move the whole queue; read from a head. Size: S.
- **P3 · Their examples miss the failure paths.** s13_process has no `write`, `closeInput` or `kill`; s12_compress no `zip.extract`, its refusal of `..` and absolute names, or zstd; s11_parse no TOML, INI or .env reads of malformed text; s3_network no peer that closes or untrusted certificate; s1_terminal no scrolling past the last row or `put`; and no example calls fs, so its `overflow` and `gone` events, `copy` and `makeFolder` are untested. Size: M.

**Tools**

- **P2 · The core's own handles use a second object system, which the JIT cannot compile.** `src/app/Bindings.c:2183` A plugin's handle is an FFI cdata made with `ffi.metatype` (`src/runtime/Services.c:1698`), but the core's panel, surface, timer and subscription are userdata whose `__index` is a C function that compares the key's text (`Bindings.c:1704`, `:2183-2215`). Measured: reading `surface.width` takes 93 ns, and `surface:fill` 216 ns, where `draw.fill` takes 87. Make the core's handles the same FFI handles as plugins', and call through FFI wrappers the core functions that never run Lua, such as a surface's `fill`, `setPixel` and `setRow`. Lua code reads the same; one handle system is left. Size: L.
