# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Bugs** that the owner reports from testing.

## Tasks

- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now, and aarch64 waits for 1.0.0 (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags.
- **Import the rulesets.** The rulesets of OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template require the Windows checks now, as the owner chose. The owner imports them in each repository's settings. OpenECS-examples' rulesets no longer require `Release (Linux)`, which it does not run.
- **Lua's speed.** Each call from Lua into the core costs about a third of a microsecond in a Debug build, so a Lua panel draws runs of pixels with `surface:fill` and rows with `setRow`, as c12_sketch's Lua panel paints each row of a dab with one `fill`; the surface's methods keep the colour of the last colour text they read. A panel that keeps its picture draws only what changed (DESIGN 5.1).

## Shuild

[Shuild](https://github.com/omerfuyar/shuild) is the build tool, a single header at `dependencies/shuild`. Shu, at `dependencies/shu`, holds the types and macros it uses. The owner allows changes to both, in their style, through pull requests into their main branches. These repositories have no CI, so a pull request is merged with a merge commit once it is tested locally: shuild's examples, and OpenECS built from scratch with its checks. Then an OpenECS pull request moves the submodule. Work found in it:

- **Windows and macOS.** Shuild names libraries for the system its compiler compiles for, and cross-compiles for Windows with MinGW (omerfuyar/shuild#23). Its Windows host code runs in OpenECS's Windows checks, which build OpenECS on Windows. `SHU_UtilRun` runs commands with the system's shell, which is `cmd.exe` on Windows.
- **A relink with nothing compiled.** The owner saw the core link again in a build that compiled 0 files. It did not happen again in builds without changes, with `-t`, after a changed `shuild.c`, or with clang. Shuild links a module again when a library it links is newer than its output. The owner looks at it later.

## Standard plugins

The plugins of [OpenECS-std](https://github.com/omerfuyar/OpenECS-std); [its DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md) describes them.

- **fs.** Watching folders on macOS reads each folder again every half second. FSEvents would tell changes as they happen.
- **The core's libraries.** The standard plugins still use the core's SDL, SDL_ttf and stb_ds (DESIGN 17.2), against the principle that plugins stand alone (OVERVIEW 12). To remove: each plugin would bring what it uses.

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
- **A showcase** in README.md of plugins and tools others build.


## Review

Recommendations from a review of the code, by area, the most important first. P1 is a bug, or a design flaw that gets harder to fix with each plugin; P2 is worth doing before 1.0.0; P3 when the code is touched. Each was checked at the lines it names; none was proven by a run. Paths under `std/` are OpenECS-std's.

**Core: communication**

- **P2 · A call from Lua into C looks up names and allocates each time.** `src/runtime/Services.c:1148`, `:2880-2888` Each handle argument and result finds its type by name twice, though the parameter already points to it; each `fn<>` argument allocates a libffi closure, a name and two registry references (`:1918-1948`); the arguments' storage is a new userdata (`:1398`). Compare the handle's type pointer, keep flags of what a signature holds from when it is parsed, and reuse callback closures. Size: M.
- **P2 · An emission is compared with every subscription of every event.** `src/runtime/Events.c:133-141`, `:174-176` Delivery runs `strcmp` over all subscriptions, and each emission allocates three times. Keep each event's subscribers in its record, which the hash map of events already holds. Size: M.
- **P2 · `ecs.handle.new` and `ecs.handle.value` take full names.** `include/ecs.lua:601,608`, `src/app/Bindings.c:425-433` The rest of `ecs.handle`, and `ECSHandle_Free`, take the plugin's local name (9.9). Take local names in both; this changes the plugin interface. Lua's `ecs.service.register` (`Bindings.c:1249`) also skips the check of `ECSIPlugin_MakeFullName` (`Plugins.c:941-945`), so `canvas.clear` in plugin canvas becomes `canvas.canvas.clear`. Size: S.
- **P2 · No example tests the paths above.** Nothing in OpenECS-examples calls `unsubscribe`, `ECSHandle_Free`, `ECSHandle_Forget` or `ECSValue_SetHandle`; c5_services' only kept callback goes from Lua to C. Add cases for a subscription changed during delivery, a handle freed while Lua holds it, and a plugin disabled while another keeps its callback. Size: M.
- **P3 · Events queued during delivery are delivered in the same pass.** `src/runtime/Events.c:342` A handler that emits on each delivery never lets the loop draw or read input. Deliver what was queued when the pass started; `ECSIEvents_GetWait` (`:356`) already starts the next pass at once. Size: S.

**std: ui, with draw**

- **P2 · C cannot tell that a tree's node was activated.** `std/plugins/ui/header.h:552-555`, `:656-673` A tree's `onActivate` gives the node's id, a string; `Ui_Activated` reads an integer, so it gives 0, and `Ui_Tree` never takes it. Give `UI_TREE` the activated id; an interface change. Size: S.
- **P2 · An element described from C gets every callback.** `std/plugins/ui/init.lua:2203-2213` `build` sets each callback of the type when the element has an id. So every C list has `onActivate`, its items are Tab stops with a hover background, and every C table's titles are clicked to sort; the same list in Lua has none of it. Set a callback only when the C side asks for it; an interface change. Size: M.
- **P2 · `UiProps` cannot say false or 0.** `std/plugins/ui/header.h:205-243` A bool is written only when true and a number only when not 0. C cannot set `stretch = false` or a group's `padding = 0`, and cannot refuse a submit, as Lua's `onSubmit` does by returning false (`init.lua:1601`). The names differ too: `invalid` and `heading` in C, `valid` and `selectable` in Lua. Give the props a third state, and Lua's names; an interface change. Size: M.
- **P2 · The same prop means different things on different elements.** `std/plugins/ui/init.lua:646-663`, `:394-487` A field's `onChange` runs at each edit, a number's at Return. One of many is `value` and `onChange` for selectors, tabs and dropdowns, and `selected` and `onSelect` for lists, tables and trees. `selected` is an index for a list and an id for a tree. In C, choices count from 0 (`header.h:479`) and list items from 1 (`:541`). Choose one model before 1.0.0; an interface change. Size: M.
- **P2 · Text in a row never wraps.** `std/plugins/ui/init.lua:737-741`, `:774` A row gives its children no room when it measures them, so a column in a row measures `wrap = true` text as one line. A column's room is also its parent's width, not its own after it grows. Measure wrapped text again once widths are arranged. Size: M.
- **P2 · Only a text's first link, and no plain scroll, takes the keys.** `std/plugins/ui/init.lua:1256-1270`, `:1765-1795` Return on a text gives its first link, so the other links of a paragraph are not reached. A `scroll` that holds no list does not scroll with the arrows, Page Up, Page Down, Home or End. Make each link a Tab stop, and let the scroll that holds the keys take those keys. Size: M.
- **P2 · `ui.show` measures everything again each frame.** `std/plugins/ui/init.lua:2082-2100`, `:758-765`, `:1151-1155` Each show forgets its fonts, reads 11 settings, and for each element reads `ecs.fontSize` and measures "Ag" for the line height, in `measure` and again in `paint`. Each text is measured in `measure` and again when it is drawn; a table's cells a third time when it is built (`:505-509`). Keep the fonts and sizes until `ecs.settingChanged`, the line height for each font and size, and each text's measured width in its layout. Size: M.
- **P2 · A long list or table is built whole each frame.** `std/plugins/ui/init.lua:536-545`, `:398-419` Each row is a node with two more for each cell, and a closure for each item with `onActivate`; all are measured and arranged, and only those in view are drawn. Rows have one height, so build and measure the rows in view, and share one handler among the items. Size: M.
- **P2 · The image cache empties when it is full.** `std/plugins/ui/init.lua:329-349` At 64 images the whole cache is dropped, so a panel that shows 65 reads them all from disk each frame. A file that failed to load stays failed until then. Drop only the images not used in the last show. Size: S.
- **P2 · Two elements with one id are not reported, and C makes them.** `std/plugins/ui/header.h:456-459`, `std/plugins/ui/init.lua:2209` A C button's id is its label, so two "Delete" buttons share one, and a click on either is taken by the one described first; the ring is drawn on both. Report an id used twice in `assignIds`, and make C's default id differ by position. Size: S.
- **P2 · Dragging a scroll bar fails when its element is gone.** `std/plugins/ui/init.lua:1886-1892` `findById` may give nil after the view changed during the drag, and `thumb(nil)` raises an error inside `ui.event`. The branches for selecting and sliding check it (`:1902`, `:1913`). Check it, and end the drag. Size: S.
- **P3 · `textgrid.cell` and `textgrid.fit` measure without the display scale.** `std/plugins/textgrid/TextGrid.c` `draw.measure` takes the scale now, and the grid draws with it, but `cell` and `fit` measure at scale 1, so their numbers can differ from the drawn cells by a part of a pixel on a display whose scale is not whole. Give them a scale parameter. Size: S.

**Core: plugins and threads**

- **P2 · Work that is dropped leaks its `data`.** `src/runtime/Tasks.c:67-73`, `:208-218` When `done` does not run, on disabling or exit, nothing frees `data`, and the plugin is not told; c12_sketch frees its export job only in `done`. Call `done` with a sign that the work was dropped, or take a function that releases `data`; either changes the plugin interface. Size: M.
- **P2 · No example disables a plugin that is working.** OpenECS-examples' `s7_first_party/test-plugins.lua:61-96` disables idle plugins only. Add a test that disables c13_workers' plugin while a worker counts, and c12_sketch's during an export, and one that enables a plugin whose start fails. Size: M.
- **P3 · A function the main thread sends itself while a plugin is disabled runs after the unload.** `src/runtime/Tasks.c:106-110` It waits in the core's queue with no plugin, and `SDL_PumpEvents` (`Plugins.c:1111`) does not deliver that queue. Deliver the core's queue before the library is unloaded. Size: S.
- **P3 · A plugin's generation is read by other threads without a lock.** `src/runtime/Tasks.c:273`, `src/runtime/Plugins.c:50` `ECS_RunInBackground` reads it from any thread, and the main thread writes it. Make it atomic. Size: S.

**Core: interface**

- **P2 · A frame draws and presents every OS window and popup.** `src/interface/Window.c:1972-1977`, `:1829-1846`, `:299-302` One panel that draws all the time makes every window, minimized and hidden behind others too, and every popup be laid out, drawn and presented at the display's rate. Mark what changed for each OS window and popup, and draw only those; skip windows that cannot be seen. Size: M.
- **P2 · Window.c holds five modules.** `src/interface/Window.c:139-211` Its 2848 lines hold the OS windows, and also the drag of panels and drop zones (`:458-730`, `:2384-2590`), the Clay declarations (`:732-1304`), the popups' views (`:1623-1848`), the title bar and hit tests, and the menus' geometry (`:2631-2765`). Move each to its own file with its part of the `WINDOW` struct; Window.c keeps the OS windows, renderers and frame pacing. Size: L.
- **P3 · The drop that waits on Wayland keeps a panel's pointer.** `src/interface/Window.c:2556` `pending.panel` outlives `ECSIWindow_Forget` for up to 400 ms, and is checked only by its address (`Layout.c:1769`). Clear it in `Forget`, or keep the panel's id. Size: S.
- **P3 · Each key press parses every binding's key text.** `src/interface/Keys.c:186`, `:230-255` Finding a binding walks the settings' tables and parses each key again. Parse them once into a table, made again when settings, the preset, workspaces or plugins change, as the prefix keys are (`:105-128`). Size: M.
- **P3 · No example presses two buttons, or sends input between a layout change and a frame.** `src/app/Test.c:845` `test.press` and `test.release` exist and no example uses them. Add cases for the two findings above that the tests can reach. Size: S.

**Core: app**


**std: panels, settings, fs and mixer**

- **P2 · The settings window does not follow changes made elsewhere.** `std/plugins/settings/init.lua:296-299` It reads the values when it is shown and after its own `set`, and subscribes to no event. A setting another plugin sets, or a plugin enabled while the window shows, is not shown, and Left and Right step from the old value. Read again on `ecs.settingChanged` and `ecs.pluginsChanged`; the panel list on `ecs.pluginsChanged`. Size: S.
- **P2 · fs loses changes on Linux without a word.** `std/plugins/fs/Fs.c:347-352` When inotify's queue overflows, its event has no name and is skipped, so no watch learns that it missed changes; std DESIGN says only Windows loses them. Report it, or tell each watch with a change such as `"overflow"`, which changes fs's interface. Size: S.
- **P2 · A watch whose folder is removed stops silently on Linux.** `std/plugins/fs/Fs.c:328`, `:352` The watch does not ask for `IN_DELETE_SELF`, and the kernel's `IN_IGNORED` is skipped. The Windows code reports it (`:461`). Report it the same way. Size: S.
- **P2 · fs gives no reason when a call fails.** `std/plugins/fs/Fs.c:160-193` `list`, `makeFolder` and `copy` give nil or false and drop SDL's error, where `fs.watch` (`:624`) and the other plugins report theirs. Report it; giving `nil, message` instead changes fs's interface. Size: S.
