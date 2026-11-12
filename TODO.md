# OpenECS TODO

What is not decided yet, and work that is waiting, for OpenECS and the repositories it ships: OpenECS-std, OpenECS-examples and OpenECS-template. When an item is settled, write the decision into the design of the repository it belongs to, and remove it from here.

## Next

What the owner wants worked on, in no fixed order. Each one is described further below.

- **Linux first.** Linux is the priority. Windows is supported; other platforms are not worked on for now, as each one needs upkeep (DESIGN 19.4).
- **Bugs** that the owner reports from testing.

## Tasks

- **Releases for other platforms.** The release workflow has entries for Linux on aarch64 and macOS, commented out; they are not worked on for now, and aarch64 waits for 1.0.0 (DESIGN 19.4). macOS would need libffi's configuration for it, and its own linker flags.
- **Import the rulesets.** The rulesets of OpenECS, OpenECS-std, OpenECS-examples and OpenECS-template require the Windows checks now, as the owner chose. The owner imports them in each repository's settings. OpenECS-examples' rulesets no longer require `Release (Linux)`, which it does not run.
- **Functions that call unseen.** OVERVIEW's principle "Calls are written out" is new. Check the core, the standard plugins and the examples for code that registers or calls functions the caller does not name, and write the calls out.
- **Older plugins and newer features.** Go over the standard plugins and the examples for code that a later feature of the core would make simpler, such as structs, and change it to use the feature.
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
  - **An editor for LÖVE games**, the owner's flagship. It is for people who want LÖVE underneath and a simpler interface than Unity or Godot. LÖVE stays the runtime; the editor is no engine. A project is scenes of objects with properties, assets and Lua scripts that the user writes in their own code editor. The editor builds the `.love` file from it, with a small Lua library that loads the scenes, and makes the executables as LÖVE's Game Distribution page says: `love.exe` with the `.love` appended on Windows, the `.love` in `love.app` on macOS, and the AppImage on Linux. It would be made from OpenECS-template, in its own repository. Open:
    - how scripts attach: a script per object with `load`, `update` and `draw`, as Unity's components do, or also logic without code, as GDevelop's events do;
    - an existing `.love` made in code has no scenes, so the editor opens its files and runs it, but edits only its own projects; or it also turns such a game into a project;
    - the scene view: OpenECS draws the sprites, shapes and text itself, and Play runs LÖVE in its own window, as Wayland does not let another program's window be embedded;
    - the LÖVE version it targets: 11.5 is the latest release, and 12.0 is not released;
    - where the editor gets LÖVE's executables for each platform, and its name, which says it is for LÖVE without using LÖVE's name as its own;
    - whether it waits for a stable plugin interface (1.0.0).
- **A showcase** in README.md of plugins and tools others build.


## Review

Recommendations from a review of the code, by area, the most important first. P1 is a bug, or a design flaw that gets harder to fix with each plugin; P2 is worth doing before 1.0.0; P3 when the code is touched. Each was checked at the lines it names; none was proven by a run. Paths under `std/` are OpenECS-std's.

**Core: communication**

- **P2 · A call from Lua into C looks up names and allocates each time.** `src/runtime/Services.c:1148`, `:2880-2888` Each handle argument and result finds its type by name twice, though the parameter already points to it; each `fn<>` argument allocates a libffi closure, a name and two registry references (`:1918-1948`); the arguments' storage is a new userdata (`:1398`). Compare the handle's type pointer, keep flags of what a signature holds from when it is parsed, and reuse callback closures. Size: M.
- **P2 · An emission is compared with every subscription of every event.** `src/runtime/Events.c:133-141`, `:174-176` Delivery runs `strcmp` over all subscriptions, and each emission allocates three times. Keep each event's subscribers in its record, which the hash map of events already holds. Size: M.
- **P3 · No example frees a handle while Lua holds it.** Nothing in OpenECS-examples calls `ECSHandle_Free`, `ECSHandle_Forget` or `ECSValue_SetHandle`, or disables a plugin while another keeps its callback. Add cases when those paths change. Size: M.

**std: ui, with draw**

- **P3 · A column in a row measures its text at its parent's width.** `std/plugins/ui/init.lua` A row narrows its wrapped texts and columns only when its children need more than its width; a column that grows in a row still breaks its text at the width it had before it grew. Measure wrapped text again once widths are arranged. Size: M.
- **P3 · A long list or table is built whole each frame.** `std/plugins/ui/init.lua:536-545`, `:398-419` Each row is a node with two more for each cell, and a closure for each item with `onActivate`; all are measured and arranged, and only those in view are drawn. Rows have one height, so build and measure the rows in view, and share one handler among the items. A list takes children that are built already, so this needs a form of `ui.list` that takes a count and a function that builds one item; the owner leaves it until a tool needs it. Size: M.
- **P3 · `textgrid.cell` and `textgrid.fit` measure without the display scale.** `std/plugins/textgrid/TextGrid.c` `draw.measure` takes the scale now, and the grid draws with it, but `cell` and `fit` measure at scale 1, so their numbers can differ from the drawn cells by a part of a pixel on a display whose scale is not whole. Give them a scale parameter. Size: S.

**Core: plugins and threads**

- **P3 · No example disables a plugin that is working.** Add a test that disables c13_workers' plugin while a worker counts, and c12_sketch's during an export, when those paths change. Size: M.
- **P3 · A function the main thread sends itself while a plugin is disabled runs after the unload.** `src/runtime/Tasks.c:106-110` It waits in the core's queue with no plugin, and `SDL_PumpEvents` (`Plugins.c:1111`) does not deliver that queue. Deliver the core's queue before the library is unloaded. Size: S.
- **P3 · A plugin's generation is read by other threads without a lock.** `src/runtime/Tasks.c:273`, `src/runtime/Plugins.c:50` `ECS_RunInBackground` reads it from any thread, and the main thread writes it. Make it atomic. Size: S.

**Core: interface**

- **P2 · A frame draws and presents every OS window and popup.** `src/interface/Window.c:1972-1977`, `:1829-1846`, `:299-302` One panel that draws all the time makes every window, minimized and hidden behind others too, and every popup be laid out, drawn and presented at the display's rate. Mark what changed for each OS window and popup, and draw only those; skip windows that cannot be seen. Size: M.
- **P3 · Window.c holds five modules.** `src/interface/Window.c:139-211` Its 2848 lines hold the OS windows, and also the drag of panels and drop zones (`:458-730`, `:2384-2590`), the Clay declarations (`:732-1304`), the popups' views (`:1623-1848`), the title bar and hit tests, and the menus' geometry (`:2631-2765`). Move each to its own file with its part of the `WINDOW` struct; Window.c keeps the OS windows, renderers and frame pacing. The owner leaves it until it is necessary. The parts call each other, so three modules in an order avoid a cycle (DESIGN 1.5): Drag, then View with every Clay declaration, then Window. Size: L.
- **P3 · The drop that waits on Wayland keeps a panel's pointer.** `src/interface/Window.c:2556` `pending.panel` outlives `ECSIWindow_Forget` for up to 400 ms, and is checked only by its address (`Layout.c:1769`). Clear it in `Forget`, or keep the panel's id. Size: S.
- **P3 · Each key press parses every binding's key text.** `src/interface/Keys.c:186`, `:230-255` Finding a binding walks the settings' tables and parses each key again. Parse them once into a table, made again when settings, the preset, workspaces or plugins change, as the prefix keys are (`:105-128`). Size: M.
- **P3 · No example presses two buttons, or sends input between a layout change and a frame.** `src/app/Test.c:845` `test.press` and `test.release` exist and no example uses them. Add cases for the two findings above that the tests can reach. Size: S.

**Shu and shuild**

Paths are in their repositories, at `dependencies/shu` and `dependencies/shuild`.

- **P3 · The settings' hash leaves the compiler's version out.** `shuild.h`, `SHUI_CacheSettingsHash` A compiler updated under the same command keeps the old objects. The configured compiler's version is not kept, and asking for it runs the compiler once more in each build. Size: S.
