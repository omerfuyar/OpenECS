# OpenECS Technical Design

This document explains how OpenECS is built: modules, interfaces, data, rules and dependencies. Read [OVERVIEW.md](OVERVIEW.md) first; this document uses its words and does not repeat it.

**How to read it:**

- Everything here is decided. Open questions and pending work are in [TODO.md](TODO.md).
- Code samples follow the conventions in section 1, but are only illustrations of an interface.
- Technical terms are explained in the glossary at the end.

---

## Contents

1. Conventions
2. Program structure
3. Main loop, timers and threads
4. Panel types and panels
5. Surfaces and rendering
6. Layout
7. Input, focus and keys
8. Events
9. Plugins
10. Services
11. Lua
12. Settings
13. Presets and sessions
14. Errors and logging
15. Memory and ownership
16. Files and directories
17. Build and dependencies
18. Platform notes
- Glossary

---

## 1. Conventions

### 1.1 Language

- C23 standard is used everywhere.
- Lua is the standard implementation, latest release.
- Out of scope: security (plugins and files are trusted), cross platform (Linux only for now) and accessibility. Look and feel is not a priority.

### 1.2 Names

| What                                    | Form                     | Example                                                 |
| --------------------------------------- | ------------------------ | ------------------------------------------------------- |
| Public type                             | `ECS` + PascalCase       | `ECSPanel`, `ECSSurface`, `ECSPanelTypeDesc`            |
| Function that belongs to a type         | `ECS<Type>_<Verb>`       | `ECSPanel_SetTitle`, `ECSLayout_Move`, `ECSTimer_Start` |
| Function that belongs to no type        | `ECS_<Verb>`             | `ECS_RunInBackground`, `ECS_Log`                        |
| Enumeration value                       | `<Type>_<Value>`         | `ECSSurfaceType_Gpu`, `ECSZone_Left`                    |
| Internal type                           | `ECSI` + PascalCase      | `ECSIPanel`, `ECSINode`, `ECSIZone`                     |
| Internal function or function-like macro | `ECSI<Type or module>_<Verb>` | `ECSIValue_Clear`, `ECSILayout_Tidy`, `ECSILua_Call` |
| Constant: macro or constant table       | `OPENECS_` + UPPER_SNAKE | `OPENECS_API_VERSION`, `OPENECS_EXPORT`, `OPENECS_CORE_FUNCTIONS` |
| Struct field, parameter, local variable | camelCase                | `stateVersion`, `minWidth`                              |
| Function pointer field                  | PascalCase               | `Create`, `Draw`, `SaveState`                           |
| Function pointer type                   | `ECS` + PascalCase + `Function` | `ECSTimerFunction`, `ECSPanelDrawFunction`       |
| Output parameter                        | `ret` + name             | `retPanel`, `retTimer`                                  |
| File                                    | PascalCase               | `OpenECS.h`, `Layout.c`                                 |
| File-level state                        | one `static` struct, UPPER_CASE| `LAYOUT`, `RENDERER`                                    |

- Internal names follow the public ones with `ECSI` in place of `ECS`. An internal enumeration value is `<Type>_<Value>` too: `ECSINodeType_Split`.
- `main.c` and Lua files, such as `manifest.lua` and presets, keep lowercase names.
- A type that tells variants apart ends with `Type`, never `Kind`: `ECSSurfaceType`, `ECSPanelEventType`. Its field is named `type`.
- Lua names use camelCase: `ecs.panel.registerType`, `saveState`, the setting `ecs.prefixKeys` and the event type `pointerDown`. The core's Lua names live in the `ecs` module (9.6). The core's own settings, events and bindable functions start with `ecs.`, for example the setting `ecs.focus`.

### 1.3 Types and results

- The core uses `shu.h` for its basic types (`i32`, `u32`, `f32`, `usz` and so on), for `SHUSlice` and `SHUSliceView`, for `SHUResult` and `SHUWUR`, and for `SHU_ReturnResult`.
- The plugin header includes `shu.h`, so plugins use the same types.
- Memory is passed as a `SHUSlice` or `SHUSliceView`, not as a separate pointer and size.
- Every function pointer type has a typedef, which fields, parameters and casts use.
- An object that plugins hold is an opaque handle: `typedef struct ECSIPanel *ECSPanel;`. Its fields stay internal.
- A function that creates an object returns `SHUResult` and writes the new handle to an output parameter, which comes first after the plugin: `SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, ...)`. Its `Destroy` or `Stop` takes a pointer to the handle and sets it to `NULL`.
- Other functions take the object they act on first. Functions that act for a plugin take the plugin first.
- Reading and changing a value use `Get` and `Set`: `ECSPanel_GetTitle`, `ECSPanel_SetTitle`.

### 1.4 Errors

- **Recoverable errors** (a missing file, a failed allocation, a bad signature string) return a `SHUResult`. `SHUResult_Ok` is zero, so `if (ECSService_GetFunction(...))` checks for failure. Functions whose result must be checked are marked `SHUWUR`.
- **Contract violations** (a `NULL` handle, an index out of range, a call from the wrong thread) are bugs. They fail an `SDL_assert`. Release builds turn these assertions off.
- Lua never reaches an assertion: the core checks every argument that comes from Lua and raises a Lua error instead (10.8).

### 1.5 Files

- Headers are in `include/`, source files in `src/`.
- `include/OpenECS.h` is the one header that plugins include. It holds every public type and function, with documentation. The build copies only this header and `include/ecs.lua` (9.6) to the build's `include/` folder, and plugins are built against that copy, so they never see the core's headers.
- Each module (2.1) is a pair of files in its group's folder: `include/<group>/<Module>.h` with the module's declarations and their documentation, and `src/<group>/<Module>.c` with the definitions. Includes name the folder: `#include "base/Values.h"`.
- `src/main.c` only reads the command line and runs the App module.
- A module includes only the modules listed before it in 2.1, so modules never depend on each other in a cycle. So a group includes only its own folder and the groups before it.
- A source file includes its own header first, then the core's other headers sorted by path, then the dependencies' headers.
- Headers hold declarations only: types, function declarations and macros. Function and variable definitions, including `static inline` functions, are in source files.
- Headers start with `#pragma once` and group their contents with `#pragma region`. A source file keeps its internal elements in a `Source Only` region.
- Functions used by only one source file are `static`.
- First-party plugins are in `plugins/<name>/`, and first-party presets in `presets/`.
- Tests are in `tests/` (17.5).
- `sketch_c` and `sketch_lua` are example plugins with the same canvases, clock, settings, services, keys, events and state, one native and one in Lua. They use every part of the plugin interface, and draw the same strokes into the same pixels.

### 1.6 Style

- Documentation comments use `///` with `@brief`, `@param`, `@return` and `@note`, on every public declaration. Each `@return` names the `SHUResult` values the function can return. A public function's comment ends with `@lua` (11.5).
- Allman braces and four spaces for indentation.
- An unused parameter is marked `(void)name;`.
- Plain comments are short and lowercase; `// todo` marks unfinished work and `//!` marks something important.
- Compiler attributes are used through macros, never written out: `OPENECS_EXPORT`, `OPENECS_PRINTF`, `SHUWUR`.

### 1.7 Dependencies

- Before writing a function, check that no dependency already has it.
- The core calls SDL directly, without wrappers, for memory, text, files and paths, shared libraries, logging and assertions: `SDL_malloc`, `SDL_strdup`, `SDL_asprintf`, `SDL_GetPrefPath`, `SDL_LoadObject`, `SDL_Log`, `SDL_assert`.
- Dynamic arrays and hash maps come from `stb_ds.h`. The core includes it through the glue header `stb/stbSDL3.h`, which sets it to use SDL's allocator and assertions. The core has no fixed limits on counts, such as the number of plugins or panels.
- Each job has one implementation, used everywhere. For example, all logging goes through SDL's log, and all memory comes from SDL's allocator, Lua's included.

## 2. Program structure

### 2.1 Modules

In order: a module includes only the modules above it (1.5). The modules are in four groups, each in its own folder.

| Group     | Module   | Job                                                                                                                 |
| --------- | -------- | ------------------------------------------------------------------------------------------------------------------- |
| base      | Log      | Where SDL's log goes, and how its lines look (14.4).                                                                |
|           | Sanitizers | The sanitizers' settings in Debug builds (17.3).                                                                  |
|           | Values   | Generic values (10.3).                                                                                              |
|           | Lua      | The Lua state. Reads data files (11.2) and runs all Lua code in protected calls.                                    |
| runtime   | Plugins  | Finding, ordering and loading plugins. Logging for plugins.                                                         |
|           | Settings | Declarations, layers, explanations.                                                                                 |
|           | Events   | Core and plugin events, the event queue, timers, worker threads.                                                    |
|           | Services | Function registry, signatures, calls between C and Lua. The only module that calls libffi.                          |
| interface | Panels   | Panel types, panels, and the pixels each panel draws.                                                               |
|           | Layout   | Workspaces and their layout trees: operations, tidying, sizes, focus, closed panels and saving.                     |
|           | Window   | OS windows and their renderers, frame pacing, and the core's own interface: tab rows, grips, menus, dragging and hit testing. The only module that calls Clay. |
|           | Keys     | Key combinations, the core prefix and the keys after it, and every key binding (7.3 to 7.5, 7.8).                  |
|           | Menus    | The core's bindable functions, and the panel and group menus that offer them (6.8).                                 |
|           | Input    | SDL's input events, focus, pointer routing, key dispatch, the list of prefix keys, text input, the clipboard, dialogs. |
| app       | Session  | Reading presets and sessions, applying them, writing them.                                                          |
|           | Bindings | The `ecs` module: the plugin interface for Lua plugins (11.3).                                                      |
|           | Test     | Runs a test in Debug builds (17.5).                                                                                 |
|           | App      | Start-up, the main loop and shutdown (2.3, 3.1, 2.4).                                                               |

### 2.2 The boundary

- No header that plugins include, and no function that plugins call, exposes a type from SDL, Lua, Clay or libffi.
- `shu.h` is the exception; it is part of the plugin interface (1.3).
- Native plugins reach SDL only through the built-in **sdl** plugin (9.8). Inside the core, every module calls SDL directly (1.7).
- Plugins can call only the core functions marked `OPENECS_EXPORT`. Every other function of the core is hidden from them (17.2).

### 2.3 Start-up

1. Read the command line (13.5) and find the preset or session.
2. Start Lua. Read the preset's identity, plugins and settings as data (11.2).
3. Give SDL the tool's identity (name, icon, app id) with `SDL_SetAppMetadata`. This happens before any OS window exists, because SDL needs the identity before it starts.
4. Start SDL, and open the main OS window and its renderer.
5. Find plugins, read their manifests, resolve dependencies and compute the load order (9.3).
6. Load each plugin in order and call its `Init`. Plugins register what they provide.
7. Build the settings layers (OVERVIEW 10.4).
8. Restore plugin state, then build the layout and panels, from this tool's last session or from the preset (OVERVIEW 10.2).
9. Report the settings and keys whose names nothing registered (7.8, 12.1).
10. Pass the files of the command line to the `open` function (13.5).
11. Enter the main loop.

### 2.4 Shutdown

1. Ask about unsaved work (4.5). The user may cancel.
2. Save the session for the next start (13.4).
3. Destroy the panels, then the objects of handles (10.6).
4. Shut plugins down in reverse load order: a plugin's Lua shutdown (`ecs.plugin.onShutdown`), then its `ECSPlugin_Shutdown`. Timers and events still work then.
5. Stop the worker threads and free the timers, then unload the plugins' libraries.
6. Stop SDL and Lua.

## 3. Main loop, timers and threads

### 3.1 The loop

The loop wakes up only when an input event arrives, a timer is due, background work finishes, or a frame is due because a visible panel draws continuously.

One pass of the loop:

1. Wait for one of the reasons above.
2. Handle input: hit testing, focus and key dispatch (7).
3. Run the timers that are due.
4. Deliver queued events (8).
5. Recompute the layout if it changed.
6. Draw the visible panels that need it: continuous panels, and panels that asked to be redrawn or changed size or scale.
7. Compose and present.

The wait uses `SDL_WaitEventTimeout` with the time until the next timer or the next allowed frame, and does not wait while events are queued.

- Continuous drawing is paced by the display's refresh (vsync). The setting `ecs.vsync` is the frame rate in percent of the refresh rate: 100 (the default) waits for every refresh, 50 for every second one, and 0 turns vsync off with no limit. A percentage that divides 100, such as 50, sets SDL's vsync interval; others keep vsync on and the core limits the frames.
- Some drivers accept vsync without waiting for it, so the core also limits frames to 1.1 times the rate that `ecs.vsync` asks for. A working vsync still sets the pace.
- A hidden, minimized or covered window is not drawn.
- Panels draw before the core declares its interface, because a panel's `Draw` may change its title, which the interface shows.

### 3.2 Timers

- Plugins ask for timers: once or repeating, with an interval. Timers run on the main thread, in step 3 of the loop.
- `ECSTimer_Start(plugin, &timer, 0.1, true, function, data)` writes a new timer into `timer`; `ECSTimer_Stop(&timer)` stops it. Lua: `ecs.timer.start(0.1, true, fn)` returns a handle, and `handle:stop()` stops it.
- `ECSPanel_StartTimer(panel, &timer, ...)` starts a timer that belongs to a panel. It stops when the panel closes, and its handle is invalid then.
- A one-shot timer ends after its function returns, and its handle is invalid then. A repeating timer that falls behind skips the calls it missed.

### 3.3 Threads

- OS windows, input, layout, event delivery, timers, every call into Lua and every callback that a plugin registers run on the main thread. SDL expects window and event calls there, and a Lua state must not be used by two threads at once.
- Background work posts its result to the main thread, which delivers it.
- `ECS_RunInBackground(plugin, work, done, data)`: `work` runs on a worker thread from a small pool, then `done` runs on the main thread. Lua code never runs on worker threads; Lua plugins use services that do their work in the background.
- The pool has one thread for each processor core but one, at most 4. It starts when background work first comes.
- A function that reaches the main thread runs between passes of the main loop, and the loop makes a pass after it. A function that the main thread sends to itself runs after the current callback returns, with the queued events (8.2).
- On exit, running work finishes and waiting work does not run, nor does its `done`.
- The thread-safe functions are: running a function on the main thread (`ECS_RunOnMainThread`, built on `SDL_RunOnMainThread`), `ECS_RunInBackground`, and logging. Every other function is for the main thread only. Each function documents its thread rule.
- Native plugins may create their own threads, under the same rule.

## 4. Panel types and panels

### 4.1 Panel type descriptor

```c
/// @brief Describes a panel type. Passed to ECSPanelType_Register.
typedef struct ECSPanelTypeDesc
{
    const char *name;        // "canvas.view": plugin name + local name
    const char *title;       // default title for tabs and menus
    u32 stateVersion;        // version of the saved state
    ECSSurfaceType surface;  // ECSSurfaceType_Pixels or ECSSurfaceType_Gpu
    bool continuous;         // draw every frame while visible
    f32 minWidth;            // in layout units, 0 for none
    f32 minHeight;

    // required
    ECSPanelCreateFunction Create;       // SHUResult (ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
    ECSPanelDestroyFunction Destroy;     // void (void *state)

    // optional, NULL if unused
    ECSPanelDrawFunction Draw;           // void (void *state, ECSSurface *surface, f64 seconds)
    ECSPanelEventFunction Event;         // void (void *state, const ECSPanelEvent *event)
    ECSPanelSaveStateFunction SaveState; // SHUResult (void *state, ECSValue *retState)
    ECSPanelSaveFunction Save;           // SHUResult (void *state): saves unsaved work
} ECSPanelTypeDesc;
```

- A Lua panel type is a table with the same fields, in Lua's naming: `create`, `draw`, `saveState` and so on.
- `Draw` receives the time in seconds since the panel was last drawn, for animation.
- `Event` returns nothing, because events never ask for permission (8.1).
- Registration: `ECSPanelType_Register(plugin, &desc)`, and in Lua `ecs.panel.registerType(desc)`.

### 4.2 Panels

- A panel has a stable id, its type, the state its type created, a title, a size, a scale, a visibility and an unsaved-work flag.
- Lifecycle: `Create` (with saved state when restoring), then `Draw` and `Event` calls, then `Destroy`.
- Functions a panel calls about itself (illustrative): `ECSPanel_Redraw`, `ECSPanel_SetTitle`, `ECSPanel_SetUnsaved`, `ECSPanel_SetCursor`, `ECSPanel_LockPointer`, `ECSPanel_SetTextInput`, `ECSPanel_AcceptDrops`, `ECSPopup_Open`.

### 4.3 When a panel is drawn

- A panel that is not continuous asks to be drawn with `ECSPanel_Redraw`. The core also draws it when its size or scale changes.
- A panel in a hidden workspace runs only when its timers or events call it.

### 4.4 Placeholders

- A panel whose callback raised an error becomes a faulted placeholder (14.2).

### 4.5 Unsaved work

- A panel sets its flag with `ECSPanel_SetUnsaved`, and a Lua panel with `panel:setUnsaved(true)`. Its tab shows a mark. Save calls the type's `Save`; if that fails, the close is cancelled.
- The question is a message dialog (`SDL_ShowMessageBox`) with Save, Discard and Cancel. When several panels have unsaved work, one dialog lists them all.
- When the dialog cannot be shown, quitting discards the work, so quitting always finishes. Closing panels is cancelled and keeps the work.

### 4.6 Popups, pointer and text input

- **Popups.** A panel opens a popup anchored to a rectangle in its own area. A popup has its own surface and closes on Escape, on a click outside it, or when its panel closes.
  - Popups are SDL popup windows (`SDL_CreatePopupWindow`). SDL keeps them inside the display and hides them with their parent. Menus take keyboard focus; tooltips do not.
- **Pointer.** The pointer's shape is a system shape or an image. The pointer lock uses `SDL_SetWindowRelativeMouseMode`; pressing the core prefix also ends it.
- **Text input.** A panel says whether it accepts text and where its text cursor is. The core turns text input on for the focused panel and tells the input method where to show its window (`SDL_StartTextInput`, `SDL_SetTextInputArea`).

## 5. Surfaces and rendering

### 5.1 Surfaces

- Pointer positions given to a panel are in its surface's pixels, so they match what it draws.
- A panel must not keep its surface after `Draw` returns; every call receives the current one. So the core can recreate surfaces, for example on resize, without telling the panel.

```c
/// @brief The picture a panel draws into. Valid only during the Draw call.
typedef struct ECSSurface
{
    ECSSurfaceType type;
    i32 width;               // physical pixels
    i32 height;
    f32 scale;               // physical pixels per layout unit

    SHUSlice pixels;         // pixels type: the whole picture
    i32 pitch;               // pixels type: bytes per row

    ECSGpuTexture texture;   // GPU type: opaque; the sdl plugin turns it into an SDL_GPUTexture
} ECSSurface;
```

### 5.2 Pixel format

- 32 bits per pixel, `ARGB8888` in native byte order, with premultiplied alpha.
- Panels are opaque, so alpha is ignored for panels. It matters only for popups.
- A panel may report which rectangles changed. If it reports none, the whole surface is uploaded.

### 5.3 The renderer

- One SDL GPU device serves every OS window. Each OS window has a 2D renderer on that device, made with `SDL_CreateGPURenderer(device, window)`.
- The core draws its own interface with that 2D renderer: Clay for layout, SDL3_ttf for text.
- A GPU surface is a texture on the shared device. Any OS window's renderer can show it by wrapping it (`SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER`), so a GPU panel moves between OS windows without recreating anything.
- A pixels surface is uploaded to a texture when it changed. Plugins with pixels surfaces never notice how the core shows them; GPU plugins depend on the **sdl** plugin's contract instead (9.8).

### 5.4 How plugins draw

- GPU panels get the GPU device and their texture through the **sdl** plugin (9.8) and draw with SDL's GPU API.
- The **ui** plugin draws with SDL's 2D renderer and SDL3_ttf. It creates an offscreen renderer on the shared device (`SDL_CreateGPURenderer(device, NULL)`) and draws into the panel's texture.
- The ui plugin draws only into GPU surfaces. Plugins that compute their own pixels use pixels surfaces.

## 6. Layout

### 6.1 Data

- Each OS window is a **root**: the OS window, its layout tree, and its maximized group, if any.
- Node types:

| Type  | Holds                                                                                                         |
| ----- | ------------------------------------------------------------------------------------------------------------- |
| Split | a direction; ordered children; for each child, a fixed size in layout units or a share of the remaining space |
| Group | ordered panels; the panel shown; whether it is locked                                                         |
| Panel | a panel                                                                                                       |

- Every panel is in a group, even when it is alone.
- A **workspace** is a name and a list of roots. A pop-out root belongs to the workspace in which it was created.

### 6.2 Tidying

After every operation:

- A split with one child is replaced by that child.
- A split inside a split of the same direction is merged into it. Its children share its place in proportion to their shares. A fixed-size split is not merged, so its children's shares keep their meaning.
- An empty group is removed.
- Node and panel ids stay stable and unique within a session.

### 6.3 Sizes

- If an OS window is too small for all minimums, the last children are clipped.
- Dragging a divider next to a fixed-size child changes that child's fixed size.

### 6.4 Grips and locked groups

- A grip appears when the pointer is within `ecs.gripZone` layout units of a panel's top edge. It shows the panel's title. While it is shown, pointer events over it go to the core.
- A locked group's grip also shows "locked". It opens the panel's menu, but it cannot be dragged, and pulling it is not a click.
- Locked groups accept no dropped panels. Their panels cannot be dragged, moved with keys or closed by the user.
- `ecs.lock` locks or unlocks the focused group.

### 6.5 Operations

- Besides the operations in OVERVIEW 6.3, there are cycling tabs, reopening the last closed panel, and moving a panel to another workspace.
- The core remembers the last `ecs.reopenLimit` closed panels with their type and saved state. `ecs.reopen` opens the last one again next to a panel that stayed in its group, wherever that panel is now, or else in the focused group, and shows its workspace.
- `ecs.moveToWorkspace1` to `ecs.moveToWorkspace10` move the focused panel into that workspace's focused group; the current workspace stays shown.
- `ecs.splitRight` and `ecs.splitDown` open another panel of the focused panel's type beside it.
- Plugins: `ECSLayout_Open`, `ECSLayout_Move(panel, target, ECSZone_Left)` (also from another workspace), `ECSLayout_Close`, `ECSLayout_Focus` and `ECSLayout_GetFocus`; `ECSWorkspace_Switch` and functions that count and name workspaces. Lua: `ecs.layout.move(panel, target, "left")` and so on, with panel handles.
- Workspaces are numbered from 1, in C and in Lua: `ECSWorkspace_Switch(10)` switches to workspace 10.
- Closing a panel from code still asks about unsaved work. Focusing a panel shows its workspace and its tab.
- Locks (6.4) stop the user, not code.
- Every change of focus tells the panel that loses it and the panel that gets it (`ECSPanelEventType_Unfocused`, `ECSPanelEventType_Focused`).

### 6.6 Placement of new panels

- The caller can name a group, a side of a panel (split), or a new OS window: `ECSLayout_Open(plugin, &panel, "text.editor", state, target, ECSZone_Right)`.
- Without a target, a new panel joins the group of the most recently focused panel of its type, or else the focused group (OVERVIEW 6.5). It opens in the current workspace and gets the focus.

### 6.7 Drop zones

While a panel is dragged, the zones are checked in this order:

1. Outside every OS window: pop out.
2. Within `ecs.dockEdge` layout units of an OS window's edge: dock along that whole edge.
3. Over a tab row: insert between tabs, at the nearest gap between tab midpoints.
4. In the outer quarter of a panel, at most `ecs.splitDepth` layout units deep: split toward that side.
5. Anywhere else over a panel: group with it.

On release, the matching operation is called. In small panels, the edge bands shrink so that the centre stays at least a third of the panel.

- A press on a tab or a grip starts a drag once the pointer moves `ecs.dragThreshold` layout units. A press on the empty part of a tab row drags the whole group the same way. Escape cancels the drag.
- A drop that would change nothing is not highlighted and does nothing: a panel on its own group or next to its own tab, a whole group on itself, or a group that fills the OS window on an edge of it.
- The pointer shows a resize arrow over a divider and while it is dragged, and a move arrow while a panel or group is dragged.
- The drop place is highlighted while the panel is dragged.
- A panel that splits another takes half of the other's place. A panel docked along an edge of the OS window takes a quarter of it.

### 6.8 Panel and group menus

- The core draws its menus itself, inside the OS window, with Clay. They are kept inside the OS window.
- A right click on a tab or grip, or a click on a grip, opens the panel's menu. A right click on the rest of a tab row opens the group's menu. The panel, or the group's shown panel, gets the focus.
- Entries are core functions that act on the focused panel. Each shows the keys that run its function after the prefix (7.5).
- Menus show only what can be done now, and say what it does now: "Lock the group" or "Unlock the group", "Maximize the group" or "Restore the group". Close and move are left out for a locked group, restart for a panel that has not failed (14.2), reopen when nothing was closed, and split when the panel's type is missing.
- Entries of one kind go into a submenu. An entry with a submenu shows `›`, and pointing at it opens the submenu beside it.
- The panel's menu: close, restart, maximize, lock, reopen the last closed panel; the submenus Split (right, down), Move (left, right, up, down) and Move to workspace (each other workspace, by number and name); then the entries of the panel's type.
- The entries of a type: `ECSPanelType_AddMenuEntry(plugin, type, function)`, or `ecs.panel.addMenuEntry(type, function)` in Lua, adds a service function whose signature is `void(handle<ecs.panel>)` or `void()`. The entry shows the function's description and the key the plugin bound to the same function for the type.
- The group's menu: the submenu Tabs, which shows any of its panels and marks the shown one with `•`; then maximize, lock, close the group's panels, and reopen.
- Up and Down choose an entry. Right or Enter opens a submenu; Left or Escape closes it. Enter runs an entry. Escape in the first menu, or a press outside the menus, closes them. While a menu is open, pointer and key events go to the menus only.
- A tab's close button and a middle click on a tab close its panel; panels of a locked group have no close button.
- The wheel over a tab row scrolls it by `ecs.tabScrollStep` layout units a step; down and right go toward the last tab. When a group shows another panel, its tab scrolls into view.

### 6.9 Clay

- The core computes the rectangles of the layout tree itself.
- Clay lays out the core's own interface on top of them: tab rows, grips, panel menus and the list of prefix keys. There is one Clay context per OS window (`Clay_SetCurrentContext`).

### 6.10 Maximize, pop-out and workspaces

- Maximize is a mark on one group. The mark is saved in the session.
- Pop-out creates an OS window with a new root that holds one group with the panel.

## 7. Input, focus and keys

### 7.1 Focus

- The focus mode is the setting `ecs.focus`.
- Keys move focus to the nearest panel in that direction that overlaps on the other axis. This does not need the tree.
- In hover mode, the pointer over a divider or a tab row does not change focus.
- On Wayland, the compositor decides whether an application may raise one of its own windows (SDL uses the xdg-activation protocol). Moving focus to another OS window is best effort there.

### 7.2 Pointer routing

- While a button is held, pointer events keep going to the panel where the press started, even outside its OS window (`SDL_HINT_MOUSE_AUTO_CAPTURE`).

### 7.3 Key combinations

- A key combination is modifiers plus one key, written as text: `Ctrl+Shift+P`. The modifiers are Ctrl, Shift, Alt and Super. The text is case-insensitive, and key names are SDL's.
- Bindings match SDL key codes, which follow the user's keyboard layout. SDL's default `latin_letters` option makes the letter keys of non-Latin layouts report English letters.
- AltGr (`SDL_KMOD_MODE`) is never part of a binding. A key pressed with AltGr held is text.

### 7.4 Dispatch

- A key press goes to the core prefix (7.5) first, then to the bindings, chosen as in OVERVIEW 7.3, then to the focused panel as a raw key event.
- A plugin binding that uses the prefix's combination is never triggered and is reported. After the prefix, the next key press goes to the core.

### 7.5 The core prefix

- The prefix is the setting `ecs.prefix`. It is one key combination, never a whole modifier.
- The keys after the prefix are the setting `ecs.prefixKeys`: a table of key combinations and the names of the functions they run. Its entries are added to its value in the core's settings file (12.1), and `false` removes a key. So presets and the user can add entries that run service functions.
- While the core waits for the key after the prefix, it lists the keys with what they do now, in sections: Navigation (focus, move, tabs and workspaces), Panel (the core's other functions) and More (service functions). Within a section, the keys follow the order of the core's functions below. Like the menus (6.8), the list leaves out what cannot be done now.
- Keys of one kind share a line. The keys that switch workspaces show the first workspace's key to the last one's: `1...0`. Focus and move show `Arrows` and `Shift+Arrows` when their four functions are on the four arrows with the same modifiers.
- The prefix and the key after it are the only key sequence the core handles.
- The core's functions after the prefix, in the order the list shows them. Their keys are the value of `ecs.prefixKeys` in the core's settings file (12.1).

  | Function                              | Action                                                                                                   |
  | ------------------------------------- | -------------------------------------------------------------------------------------------------------- |
  | `ecs.focusLeft` and so on             | Move focus                                                                                               |
  | `ecs.moveLeft` and so on              | Move the focused panel into the neighbouring group, or along that edge of the OS window if there is none |
  | `ecs.nextTab`                         | Show the next tab                                                                                        |
  | `ecs.maximize`                        | Maximize or restore                                                                                      |
  | `ecs.popOut`                          | Pop out                                                                                                  |
  | `ecs.close`                           | Close the panel                                                                                          |
  | `ecs.closeGroup`                      | Close the group's panels                                                                                 |
  | `ecs.lock`                            | Lock or unlock the group                                                                                 |
  | `ecs.reopen`                          | Reopen the last closed panel                                                                             |
  | `ecs.restart`                         | Restart the failed panel                                                                                 |
  | `ecs.workspace1` to `ecs.workspace10` | Switch to workspace 1 to 10                                                                              |

- Escape after the prefix cancels. It is not a function, so it always works.

### 7.6 Clipboard

- `ECSClipboard_SetText` and `ECSClipboard_GetText` move text; `ECSClipboard_SetData` and `ECSClipboard_GetData` move data of a MIME type, such as `image/png`. Lua: `ecs.clipboard.setText`, `getText`, `setData` and `getData`.
- The core copies what it puts on the clipboard. What a getter returns stays valid until the next clipboard call.

### 7.7 Dialogs

- `ECSDialog_Show(plugin, &desc)` shows an open file, save file or open folder dialog, with filters such as `{ "Images", "png;jpg" }`. It does not wait: the description's `Done` function gets the paths on the main thread, or `NULL` when the user cancels or the dialog fails. Lua: `ecs.dialog.show(desc, function(files) end)`.
- `ECSDialog_ShowMessage(title, message, buttons, count, &button)` waits for the user. Enter presses the first button and Escape the last. Lua: `ecs.dialog.message(title, text, buttons)`.

### 7.8 Binding keys

- Keybindings are settings of type `key`.
- Plugins have no function for workspace or global bindings.
- `ECSKey_Bind(plugin, panelType, settingName, functionName)`, and in Lua `ecs.input.bind(panelType, settingName, functionName)`: the plugin's key setting holds the key, and the key runs a registered function (10). The binding counts in the layer that sets the key setting (OVERVIEW 7.3).
- Presets bind keys with `keys` tables for the whole tool and for each workspace (13.2); the user's files with `keys` tables for every tool and for one tool (12.3). The tables map key combinations to function names.
- Once the session is built, a key of these tables or of `ecs.prefixKeys` that runs a function its owner does not have is reported, if the owner runs (12.1).
- A function bound by name takes no arguments, or one argument: the focused panel. Its signature is `void()` or `void(handle<ecs.panel>)`.
- The core registers its own bindable actions as functions under `ecs`, for example `ecs.focusLeft` and `ecs.maximize`. So settings name them like any plugin function.

## 8. Events

### 8.1 Types

- A panel gets the events about itself (`ECSPanelEvent`): input (key, pointer, wheel, text, drag and drop), focus, and whether it is shown, hidden or resized.
- Named events carry a value (10.3). The core emits its own (8.4), and plugins declare and emit theirs (8.3).
- Events are notifications. Handlers return nothing and cannot cancel anything.
- A panel gets `Shown` when it becomes visible and `Hidden` when another tab, workspace or maximized group hides it, and `Resized` when its size changes while it is visible. `Shown` and `Resized` carry the size in layout units. The layout checks after each pass, so a panel that was never visible gets no `Hidden`.
- A panel's event is a tagged union, `ECSPanelEvent`: its type chooses which member is set, `pointer`, `wheel`, `key` or `size`. Every input event carries the modifiers held when it happened. A wheel amount is positive away from the user, even when the system flips the wheel.

### 8.2 Delivery

- Events are queued and delivered on the main thread after the current callback returns, never inside another event handler.
- Changes take effect immediately; only the notification waits. A closed panel leaves the layout at once, but its `Destroy` runs after the current delivery finishes, so no handler meets a destroyed panel.

### 8.3 Named events

- A plugin declares the named events it emits: `ECSEvent_Declare(plugin, "canvas.selectionChanged", description)`. The name starts with the plugin's name. Only the declaring plugin emits it: `ECSEvent_Emit(plugin, name, value)`. The value may be `NULL`; the core copies it.
- `ECSEvent_Subscribe(plugin, name, &subscription, fn, data)` calls `fn(data, name, value)` for each emission; `ECSEvent_Unsubscribe` ends it. A subscriber hears the core's events, its own, and those of plugins its manifest depends on. The event must be declared first, so a plugin subscribes after its dependencies have loaded.
- Emissions are queued like every other event (8.2).
- When a plugin fails, its events, its subscriptions and the subscriptions to its events are removed.
- Lua: `ecs.event.declare(name, description)`, `ecs.event.emit(name, value)` and `ecs.event.subscribe(name, fn)`, which returns a subscription with `cancel()`. `fn` gets the name and the value.

### 8.4 The core's named events

Panels are named by their id (`ECSPanel_GetId`, `panel:getId()`); `ECSLayout_FindPanel(id)` and `ecs.layout.find(id)` find them.

| Event                   | Value                              | When                                                    |
| ----------------------- | ---------------------------------- | ------------------------------------------------------- |
| `ecs.panelOpened`       | `{ panel = id, type = name }`      | A panel enters the layout, also when a session is built |
| `ecs.panelClosed`       | `{ panel = id, type = name }`      | A panel leaves the layout                               |
| `ecs.focusChanged`      | `{ panel = id }`, or `{}` for none | Another panel gets the focus                            |
| `ecs.workspaceSwitched` | `{ workspace = number }`           | Another workspace is shown                              |
| `ecs.layoutChanged`     | nil                                | Panels are moved, grouped, closed, maximized or locked  |

## 9. Plugins

### 9.1 On disk

```
plugins/
  canvas/
    manifest.lua   -- description and dependencies
    canvas.so      -- native code (optional)
    init.lua       -- Lua code (optional)
```

- Paths in a manifest are relative to the manifest.
- A plugin may have both kinds of code. The native `ECSPlugin_Init` runs first, then `init.lua`.

### 9.2 Manifest

```lua
return {
  name = "canvas",            -- unique; the prefix of everything it registers
  version = "1.2.0",          -- semantic version
  api = 1,                    -- plugin API version it was built for
  description = "Paint canvas",
  depends = { ui = "1.0" },   -- plugin name = minimum version
  native = "canvas.so",       -- optional
  lua = "init.lua",           -- optional
}
```

- Versions follow semantic versioning. A dependency's version means "this version, or a later one with the same major number": `"1.2"` accepts 1.2.0 up to, but not including, 2.0.0.
- Manifests are read as data, without running plugin code (11.2).
- A manifest describes the plugin to the core. It holds no build settings.
- Presets follow the same rules as manifests: they name the plugins they depend on.

### 9.3 Search path and load order

- Search order: the preset's directory, the user's plugin directory, then the first-party plugins shipped with OpenECS (16). The first plugin found with a given name wins, so a user can replace a first-party plugin.
- The load order is a topological order of the dependencies. Plugins affected by missing dependencies, version mismatches or cycles are skipped (14.2).

### 9.4 Native plugin interface

- The plugin API version in the manifest is checked before the plugin's library is loaded.
- A native plugin calls the core's functions directly: `ECSPanel_SetTitle(panel, "main.c")`. The calls are resolved when the plugin's library is loaded, against the functions that the executable exports (17.2).
- A native plugin exports `ECSPlugin_Init` and, if it needs one, `ECSPlugin_Shutdown`:

```c
/// @brief Every native plugin defines this function. The core calls it once, after it loads the plugin's library.
OPENECS_EXPORT SHUResult ECSPlugin_Init(ECSPlugin plugin);

/// @brief A native plugin may define this function. The core calls it once, before the program exits.
OPENECS_EXPORT void ECSPlugin_Shutdown(ECSPlugin plugin);
```

- `ECSPlugin_Init` receives the plugin's handle. Functions that act for the plugin take it first, so the core knows who is calling.
- A plugin registers things only inside `ECSPlugin_Init`.
- Plugins are loaded with `SDL_LoadObject` and `SDL_LoadFunction`.

### 9.5 ABI rules

- Within one plugin API version, the public types and functions, including those from `shu.h`, do not change. A change raises `OPENECS_API_VERSION`, and the core refuses plugins made for another version (14.2).
- Adding a function needs no new version.
- Plain C only: no C++ exceptions and no `longjmp` across the boundary.

### 9.6 Lua plugins

- All Lua plugins share one Lua state. Each plugin's code runs in its own environment, which reads globals from the shared global table.
- A plugin gets the core's Lua names from the `ecs` module: `local ecs = require("ecs")`. There is no global `ecs`.
- The environment has its own `require`. For `"ecs"` it gives the plugin's own `ecs` table, whose functions carry the plugin, so the core knows which plugin made an `ecs` call. Other names go to Lua's `require`.
- `include/ecs.lua` describes the `ecs` module for editors: every function's parameters, results and documentation, in LuaLS annotations. It changes with the bindings. A plugin's author adds the build's `include/` folder to `workspace.library` in the plugin's `.luarc.json`.
- The manifest's `lua` file runs once, in a protected call, after the native `ECSPlugin_Init`. An error fails the plugin.

### 9.7 Lifecycle

- Native plugins are never unloaded before exit, because their function pointers, threads and static data may still be in use.
- The core records everything each plugin registers. If `ECSPlugin_Init` fails, the plugin's registrations are removed and it is marked failed.

### 9.8 Built-in sdl plugin

- The executable contains a binding plugin named `sdl`, registered like any plugin. Its service gives native plugins SDL objects: the GPU device, a panel's GPU texture, and the SDL version.

### 9.9 Names

- Everything a plugin registers is named `<plugin name>.<local name>`: functions (`canvas.new`), panel types (`canvas.view`), events, settings (`canvas.grid`) and keybindings. The core rejects other names.
- Core code never checks a plugin's name to decide what to do. First-party plugins get no special treatment.

## 10. Services

### 10.1 Registration

- A plain C function can be registered without a hand-written wrapper.
- Each function also has a one-line description, so menus and key-binding editors can show it.
- In C, a function of any type is passed as `ECSFunction` and cast back to its real type by the caller.

```c
ECSService_RegisterFunction(plugin, "audio.play", (ECSFunction)AudioPlay, "int(string, float)", "Play a sound file");
```

```lua
local ecs = require("ecs")

ecs.service.register("audio", {
  play = { sig = "int(string, float)", doc = "Play a sound file",
           fn = function(path, volume) ... end },
})
```

### 10.2 Signature types

| Type              | Meaning                                            |
| ----------------- | -------------------------------------------------- |
| `void`            | no value (return only)                             |
| `bool`            | true or false                                      |
| `int`, `int64`    | whole numbers                                      |
| `float`, `double` | decimal numbers                                    |
| `string`          | text ending in a zero byte                         |
| `buffer`          | a `SHUSlice`, passed by value                      |
| `handle<name>`    | a typed handle (10.6)                              |
| `value`           | a generic value (10.3): a `const ECSValue *` in C  |
| `fn<signature>`   | a function to call back (10.4)                     |
| `out <type>`      | an output parameter; in Lua, an extra return value |

- Structures are passed as handles or buffers, never by value.
- In C, an `out` parameter is a pointer to its type. An `out value` is a value that the caller gives and the function fills.
- There is no fixed limit on the number of parameters.

### 10.3 Values

A generic value (`ECSValue` in C) is nil, a boolean, an integer, a number, a string, a buffer, a handle, or a table (a list or named fields). Saved state, settings, plugin events and generic calls use values.

- A plugin fills a value that the core gives it, with `ECSValue_SetInteger`, `ECSValue_TableSetField` and so on. To pass a value of its own, a plugin makes it with `ECSValue_Create` and destroys it with `ECSValue_Destroy`.
- Getters take a fallback, returned when the value has another type or is missing: `ECSValue_GetInteger(ECSValue_GetTableField(state, "document"), 0)`. So saved state from an older or edited file never needs extra checks.
- Each part of a value is allocated on its own with SDL's allocator. Values are small and short-lived, so they need no arena.
- A table is a list and named fields together. From Lua, integer keys from 1 up to the first missing one are the list, and text keys are the fields. Other keys are reported and skipped.
- Functions on a table's list have `List` in their names (`ECSValue_GetListCount`, `ECSValue_GetListItem`, `ECSValue_ListAddItem`), and functions on its named fields have `Table` (`ECSValue_GetTableField`, `ECSValue_TableSetField`).

### 10.4 Calls

- **Lua calls C:** the core makes one Lua function for each registered C function. It converts the arguments by the signature, calls the C function through libffi, and converts the results back.
- **C calls C:** the caller gets the raw function pointer and calls it at full speed.
- **C calls Lua:** the caller also gets a typed C function pointer: a libffi closure that converts the arguments, calls the Lua function in a protected call, and converts the result.
- **Lua calls Lua:** a plain Lua call, because all plugins share one Lua state.
- Callbacks (`fn<...>` parameters) work the same way in both directions. In C, a callback is a function pointer of its signature; in Lua, a function. `nil` in Lua is `NULL` in C.
- A callback is valid only during the call that gives it. C must not keep it; a Lua function that keeps one gets an error when it calls it later.
- A callback is a parameter only, never `out` or a result, and its own signature has no callbacks.
- Strings, buffers and values that a Lua function gives to C stay valid until that function returns again.
- When a Lua function called from C raises an error or gives a value of the wrong type, the error is reported (14.2) and C gets zeros.

### 10.5 Lookup

- A plugin asks for a function by name and states the signature it expects. The core compares it with the registered signature and refuses a mismatch, so a version mismatch shows up at lookup instead of crashing a call.
- A plugin may look up its own functions, and functions of plugins named in its manifest's dependencies. A provider fails only while it loads, before the plugins that depend on it, and those are skipped (14.2). So a plugin never uses a failed provider.
- C: `ECSService_GetFunction(plugin, &play, "audio.play", "int(string, float)")`. Lua: `ecs.service.get("audio.play", "int(string, float)")`, where the signature may be left out.

### 10.6 Handles

- A handle stands for an object owned by its provider: a pointer plus a type name and a destructor, registered with `ECSHandle_RegisterType(plugin, "audio.sound", Destroy)`. In C, a `handle<audio.sound>` is the object's pointer.
- In Lua, a handle is a userdata whose metatable names its type. A handle of the wrong type is rejected with a clear error. When Lua no longer uses a handle, its garbage collector calls the destructor.
- The same object always has the same Lua handle. A provider that still uses an object after giving it to Lua counts references, and its destructor drops one.
- The core's own handle type is `ecs.panel`: Lua's panel handles (11.4).
- A Lua plugin provides a handle type too: `ecs.handle.registerType(name)`, `ecs.handle.new(name, value)` for a handle that stands for a Lua value, and `ecs.handle.value(handle, name)`, which gives the value back to the plugin that owns the type. Users see such a handle like any other. The core keeps the value until the handle is collected.
- Handles that wait for their finalizer at exit are collected before the handle types are freed.
- A failed plugin's handles become invalid without their destructor. On exit, the objects of handles that Lua still holds are destroyed before plugins shut down.

### 10.7 Buffers

- A buffer argument is valid only during the call. A function that needs it longer copies it.
- A returned buffer's lifetime is documented by its function. Lua callers always receive a copy.

### 10.8 Errors in services

- In C, a `SHUResult`, like every core function (1.4). In Lua, expected failures, such as a missing file, return `nil, message`; misuse, such as a wrong argument type, raises an error.

### 10.9 libffi

- Each function's call description is prepared once, at registration.
- The core cannot check that a C function really matches its declared signature. A wrong declaration is a bug in the plugin and may crash the program.

## 11. Lua

### 11.1 Protected calls

Every call from the core into Lua is a protected call. A caught error becomes an error report (14), never a crash.

### 11.2 Data files

- Manifests, presets, sessions and settings files are read as data. They run without `ecs`, so they cannot call the core or plugins while they are read.
- They are loaded as text only (`load(text, name, "t", env)`). The environment holds Lua's basic functions and the `string`, `table`, `math` and `utf8` libraries; there is no `io`, `os` or `require`. So all of these files can use loops and conditions.

### 11.3 The `ecs` module

| Table                         | Contents                            |
| ----------------------------- | ----------------------------------- |
| `ecs.panel`                   | panel types and panel functions     |
| `ecs.layout`                  | layout operations                   |
| `ecs.workspace`               | workspaces                          |
| `ecs.input`                   | key bindings and focus              |
| `ecs.event`                   | declare, emit and subscribe         |
| `ecs.handle`                  | handle types of Lua plugins (10.6)  |
| `ecs.timer`                   | timers                              |
| `ecs.service`                 | register and look up functions      |
| `ecs.settings`                | declare, get, set, list and explain |
| `ecs.session`                 | save and load                       |
| `ecs.plugin`                  | information about plugins           |
| `ecs.clipboard`, `ecs.dialog` | clipboard and dialogs               |
| `ecs.log`                     | `debug`, `info`, `warn` and `error` |

- `ecs.plugin` holds the plugin's `name` and `version`, `registerState` (13.2), and `onShutdown(fn)`, the Lua counterpart of `ECSPlugin_Shutdown` (2.4).

### 11.4 Panels in Lua

- A Lua panel type is a table (4.1). The core registers C callbacks that call its Lua functions in protected calls.
- Panels are handles with methods: `panel:redraw()`, `panel:getId()`, `panel:getType()`, `panel:getTitle()`, `panel:setTitle(text)`, `panel:setUnsaved(unsaved)` and `panel:startTimer(seconds, repeat, fn)`. Each is also a function of `ecs.panel` that takes the panel first, such as `ecs.panel.getTitle(panel)`. A handle of a destroyed panel raises an error when it is used.
- `draw(state, surface, seconds)` gets a surface with `width`, `height` and `scale`, and the methods `setPixel(x, y, color)`, `getPixel(x, y)` and `setRow(y, bytes, x)`. Colours are ARGB integers; `setRow` takes the row's pixels as a string in native byte order. Pixels outside the surface are clipped. A surface is valid only during the call.
- `event(state, event)` gets a table: `type` (such as `"pointerDown"`), the booleans `shift`, `ctrl`, `alt` and `super`, and the fields of its type: `x` and `y` for pointer and wheel events, `button` for pointer presses, `wheelX` and `wheelY` for the wheel, `key` (SDL's key name) for keys, and `width` and `height` for `shown` and `resized`.

### 11.5 Parity

- The documentation of each public function in `OpenECS.h` ends with a line that names its Lua counterparts in `include/ecs.lua`, such as `/// @lua ecs.panel.getTitle, panel:getTitle`, or says `none:` and why.
- The functions of values (10.3) have no `@lua` line, because Lua passes its own values.
- A function of `ecs.lua` that C does not have says `Lua only:` and why in its documentation.
- The test `tests/parity.lua` checks both files: every public function has its line, every name it gives is in `ecs.lua`, and every function of `ecs.lua` is named or says it is Lua only.

### 11.6 Keeping Lua values

Lua functions and values that C code keeps are stored in Lua's registry and referenced by a number. They are released when their owner goes away.

## 12. Settings

### 12.1 Declaring

- Plugins declare settings with a name, a type, a default and a description: `ECSSetting_Declare(plugin, &desc)`. The core declares its own with a name, a type and a description; their defaults are in the core's settings file. Owners are told when their settings change: the description's `Changed` function runs after the queued events (3.1, step 4), and only when the value in effect really changed.
- Types: `bool`, `integer`, `number`, `string`, `choice` (one of a list), `key` (a key combination), `list` and `table`.
- The core's settings hold the core's choices of look and behaviour: the keys, focus and vsync, the window's size, font and colours, the sizes of tab rows, dividers and grips, the distances that start drags and drops, and the reopen limit.
- The core's settings file is `resources/settings.lua` next to the executable, in the format of the user's file (12.3). It is the core layer, and the only place that gives the core's settings their defaults. OpenECS does not start if the file cannot be read, or if it gives a core setting no value of its type. Its `ecs.prefix` must be a key combination, because a prefix that cannot be read falls back to it.
- `ECSSetting_Get(name)` returns the value in effect as a value (10.3). It comes from the highest layer that sets the setting with a value of its type; otherwise it is the default. A value of another type is reported with its file and skipped.
- The core reads a key combination when it uses it. A key text that cannot be read is reported, and the default is used.
- A name's owner is the text before its first dot: `ecs` for the core, or a plugin. Once the plugins are loaded, a setting in a file that is not declared is reported with its file if its owner runs: the core, or a plugin that loaded and did not fail. So a misspelt name is noticed. The setting is kept (OVERVIEW 12).

### 12.2 Interface

- `get(name)`, `set(name, value)` (writes the settings window's file), `list()` (every declared setting) and `explain(name)`: the value in effect, the layer it came from, and what each layer says. In C: `ECSSetting_Get`, `ECSSetting_Set`, `ECSSetting_List` and `ECSSetting_Explain`; lists and explanations are values.
- `set` changes the tool's own part of the settings window's file if that part already has the setting; otherwise it changes the part for every tool.
- The settings window plugin uses `list` and `explain`.

### 12.3 User files

The part for one tool is keyed by the tool's app id and wins over the general part of the same file. A field whose name has a dot is a setting. The settings window's file has the same shape.

```lua
-- ~/.config/openecs/settings.lua
return {
  ["ecs.focus"] = "hover",
  keys = { ["Ctrl+Alt+T"] = "terminal.open" },   -- bindings for every tool
  plugins = { "my-scripts" },                     -- extra plugins for every tool
  tools = {
    ["org.example.Paint"] = { ["canvas.grid"] = false },
  },
}
```

## 13. Presets and sessions

### 13.1 Format

Presets and sessions share one format: a Lua file that returns a table.

### 13.2 Fields

```lua
return {
  format = 1,                                -- version of this file format
  name = "paint",
  version = "1.0.0",
  app = { id = "org.example.Paint", name = "Paint", icon = "paint.png" },
  depends = { canvas = "1.2", palette = "1.0" },
  pluginsDir = "plugins",                    -- optional extra plugin directory
  open = "canvas.open",                      -- receives files from the command line
  settings = { ["canvas.grid"] = true },
  keys = { ["Ctrl+N"] = "canvas.new" },      -- bindings for the whole tool
  workspaces = {
    { name = "drawing",
      windows = {                            -- one layout tree per OS window
        { split = "horizontal",
          { size = 240, panels = { { type = "palette.view" } } },
          { share = 1,  panels = { { type = "canvas.view", state = { document = 1 } } } },
        },
      },
    },
  },
}
```

- A layout node is a split (`split` plus its children) or a group (`panels`, and optionally `shown` and `locked`). A split's child has a fixed `size` in layout units or a `share`.
- A workspace can have its own `keys`.
- A panel's saved state is its `state` field, and the state's version is `stateVersion`.
- Sessions also store each panel's `id`, each workspace's `focus` (a panel id), each group's `shown` panel and `maximized` mark, the `currentWorkspace`, and `pluginState`: for each plugin's name, its `state` and `stateVersion`.
- A plugin saves state of its own, apart from its panels', with `ECSPlugin_RegisterState(plugin, &desc)`: a version and `Save` and `Restore` functions. Lua: `ecs.plugin.registerState({ version = 1, save = fn, restore = fn })`. The state of a plugin that is not loaded stays in the session.
- The app id matches the name of the tool's `.desktop` file.

### 13.3 Applying a session

1. Read the file as data (11.2) and check its shape. An error names the path of the problem, for example `workspaces[1].windows[2]`.
2. Make sure the plugins it needs are active.
3. Restore each plugin's state, with its version.
4. Build the layout hidden, and create each panel with its saved state.
5. Show everything at once.

The core converts only layout data.

### 13.4 Saving

- Only data is written: numbers, strings, booleans and tables. Functions and reference cycles are an error.
- The writer is a short Lua function embedded in the executable. It uses `string.format("%q", x)`, so strings read back exactly. Numbers are written in the shortest form that reads back exactly.
- Named fields are read and written in the order of their names, so the same session always writes the same file.
- A session is written from the file it came from, with the current workspaces. So fields that the core does not use are kept.
- A file is written to a temporary file, then renamed over the old one, so it is never left half-written.

### 13.5 Command line

```
openecs [--preset NAME|FILE] [--session FILE] [--fresh] [--test FILE] [FILE...]
```

- `--fresh` starts from the preset instead of the tool's last session.
- `--test` runs a test (17.5).
- A session's identity wins over the preset's, so the session is saved again as the last session of its own tool.
- Files are passed to the function that the preset or session names in `open`. Its signature is `void(string)`. It is called once for each file, in order, once the session is built (2.3). Without an `open` function, the files are reported and not opened.
- A tool's `.desktop` file runs, for example, `openecs --preset paint %F`.

## 14. Errors and logging

### 14.1 Error reports

- A native plugin that crashes takes the program down. This cannot be prevented and is accepted.
- An error report holds the plugin, the type of error, a message and, for Lua, a stack trace. It goes to the log. The user sees it where it applies: a faulted panel shows it; plugin loading problems appear in the launcher and the log. A message dialog is used only when start-up fails.

### 14.2 Policies

| Situation                                                                             | What happens                                                                                                                                       |
| ------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Manifest unreadable or invalid                                                        | The plugin is skipped and reported, and so are its dependents.                                                                                     |
| Missing dependency or version mismatch                                                | The plugin and its dependents are skipped and reported. The report names what needs the plugin (a plugin, the preset, a session or the user's settings) and, for a missing plugin, each path that was looked in. |
| Dependency cycle                                                                      | The plugins in the cycle are skipped and reported.                                                                                                 |
| Plugin API version mismatch                                                           | The plugin is refused before any of its code runs.                                                                                                 |
| `ECSPlugin_Init` or the plugin's Lua file fails                                       | The plugin is marked failed and its registrations are removed. Plugins that depend on it are skipped and reported.                                 |
| Invalid registration (bad signature, bad or duplicate name, the core prefix as a key) | That registration is rejected and the error is returned to the plugin, which continues.                                                            |
| A panel callback raises an error                                                      | The panel becomes a faulted placeholder that shows the error. Its menu has a "Restart" entry (`ecs.restart`), which recreates the panel in place from its last saved state: the state the session last saved, or the one it was created with. |
| A plugin callback (timer, event handler) raises an error                              | The error is reported. Repeats of the same error are counted, not reported again.                                                                  |
| Problems while restoring a session                                                    | See 13.3.                                                                                                                                          |

### 14.3 Error convention

- C errors follow 1.4; Lua errors follow 10.8.
- The core logs the details of each error, so a result code needs no message.

### 14.4 Logging

- Levels: error, warning, info and debug. Debug builds show every level; other builds show info and above. A message that is not shown is not formatted.
- All logging goes through SDL's log, including SDL's own messages. The core calls it directly; plugins call `ECS_Log`, which adds the plugin's name.
- Each line holds the time, the level, the plugin and the message: `12:30:05.123 warning  [canvas] message`. The core's own lines name `ecs`.
- Lines go to standard error and to the log file (16). Each start writes the log file anew.

## 15. Memory and ownership

- Whoever allocates memory frees it. The core never frees plugin memory, and never returns memory that a plugin must free.
- Strings passed into a function are valid during the call. Keep a copy to use them later.
- Memory returned by the core stays valid until a documented point, for example "until the next call".
- Every public function documents who owns what it takes and returns.
- No shared allocator is needed, because ownership never crosses the boundary.

## 16. Files and directories

OpenECS follows the XDG Base Directory specification:

| What                            | Where                                                                     |
| ------------------------------- | ------------------------------------------------------------------------- |
| The user's hand-edited settings | `$XDG_CONFIG_HOME/openecs/settings.lua` (default `~/.config`)             |
| The settings window's file      | `$XDG_CONFIG_HOME/openecs/settings-window.lua`                            |
| The user's presets              | `$XDG_CONFIG_HOME/openecs/presets/`                                       |
| The user's plugins              | `$XDG_DATA_HOME/openecs/plugins/` (default `~/.local/share`)              |
| Saved sessions (default folder) | `$XDG_DATA_HOME/openecs/sessions/`                                        |
| A tool's last session           | `$XDG_STATE_HOME/openecs/<app id>/session.lua` (default `~/.local/state`) |
| The log                         | `$XDG_STATE_HOME/openecs/openecs.log`                                     |
| First-party plugins and presets | `plugins/` and `presets/` next to the executable                          |
| The core's settings             | `resources/settings.lua` next to the executable                           |

- The data folders come from `SDL_GetPrefPath`, which follows `XDG_DATA_HOME`. SDL has no function for the configuration and state folders, so the core reads `XDG_CONFIG_HOME` and `XDG_STATE_HOME` itself.

## 17. Build and dependencies

### 17.1 Dependencies

| Library                     | Status                                                |
| --------------------------- | ----------------------------------------------------- |
| SDL3                        | core                                                  |
| SDL3_ttf                    | core: text in the core's interface                    |
| Clay                        | core: the core's own interface                        |
| Lua                         | core                                                  |
| libffi                      | core: calls by signature                              |
| shu                         | core: basic types and results                         |
| stb (`stb_ds.h`)            | core: dynamic arrays and hash maps                    |
| SDL3_image                  | not used; the core loads PNG files with `SDL_LoadPNG` |
| SDL3_mixer, SDL3_net, cgltf | not core; plugins may use them                        |

### 17.2 Linking

- SDL3 and SDL3_ttf are shared libraries shipped next to the executable and found through an `$ORIGIN` run path. So the core, the sdl plugin and any plugin that links an SDL library, such as SDL3_mixer or SDL3_net, share one copy of SDL.
- Lua, Clay, libffi and stb are linked statically into the executable.
- libffi is compiled without its configure script. Its configuration is a glue header for Linux on x86_64 and aarch64, `dependencies/other/libffi/fficonfig.h`, and the build makes `ffi.h` from libffi's template.
- The executable exports the functions marked `OPENECS_EXPORT` and nothing else. The core is compiled with hidden symbols by default.
- Native plugins are shared libraries, built against the plugin header only. They do not link against the core; their calls to it are resolved when they are loaded.
- A first-party plugin is built from every C file in its folder, `plugins/<name>/`, so the build needs no settings for each plugin.

### 17.3 Compiler

- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.
- Debug builds of the core and of plugins add the static analyzer (`-fanalyzer`) and the address, leak and undefined-behaviour sanitizers (`-fsanitize=address,undefined`). Dependencies get neither.
- The Sanitizers module sets the sanitizers' options and hides leaks inside the system libraries that SDL loads: graphics drivers, display servers, input methods and D-Bus. In other builds its functions do nothing.
- Debug builds keep every library loaded until the program exits, so a leak inside one is matched by its name. Graphics drivers are otherwise unloaded when their device is destroyed, and their leaks would show an unknown module.
- Debug builds check for leaks at shutdown, after OpenECS has freed its memory and before `SDL_Quit`.
- Release builds set `SDL_ASSERT_LEVEL` to 0, so they have no assertions. Debug builds set it to 2.

### 17.4 Dependency versions

Every dependency is a git submodule pinned to a release tag, not to a development commit.

### 17.5 Tests

- A test is a Lua file in `tests/`. Debug builds run it with `openecs --test FILE`; other builds refuse the option.
- The file returns a table: `preset`, the preset to start from (a name, or a path relative to the test file; `default` if left out), `files`, paths relative to the test file that are opened as if the command line named them (13.5), and `run`, a function that gets the `test` table.
- Tests keep their presets and test plugins in `tests/presets/` and `tests/plugins/`, so a change to a first-party preset does not change a test.
- A test starts from its preset alone. It reads none of the user's settings, plugins or sessions, and writes no session and no log file. SDL's offscreen video driver and software renderer are the defaults, so a test needs no display; the variables `SDL_VIDEO_DRIVER` and `SDL_RENDER_DRIVER` choose others, for example to watch a test.
- `run` is a coroutine in the main loop. A function that sends input or waits pauses it. Each input event gets its own pass of the loop, and `run` goes on when the last one is handled, its events are delivered and the window is drawn. While a test runs, frames are not paced (3.1).
- The `test` table:

  | Function                                       | Does                                                                                                     |
  | ---------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
  | `key(combination)`                             | Presses and releases a key combination, such as `"Alt+W"` (7.3).                                         |
  | `move(x, y)`                                   | Moves the pointer. Positions are in layout units of the OS window.                                       |
  | `press(x, y, button)`, `release(x, y, button)` | Presses or releases a button: 1 left (the default), 2 middle, 3 right.                                   |
  | `click(x, y, button)`                          | Presses and releases a button.                                                                           |
  | `drag(x, y, toX, toY)`                         | Presses the left button, moves in steps and releases it.                                                 |
  | `wheel(x, y, amount)`                          | Turns the wheel; a positive amount is away from the user.                                                |
  | `call(name)`                                   | Runs a bound function (7.8), such as `"ecs.maximize"`, as a key would.                                   |
  | `wait(seconds)`                                | Lets the program run, for timers. Without seconds, it waits one pass of the loop.                        |
  | `session()`                                    | The session that quitting would save now, as a Lua table (13.2).                                         |
  | `rect(id)`                                     | The rectangle of the shown panel with that id: `x`, `y`, `width` and `height`.                           |
  | `screenshot(path)`                             | Draws a frame and saves it as a PNG file.                                                                |
  | `match(actual, expected, message)`             | Checks that `actual` has the same number of list items as `expected`, and each item and field it names. |

- `match` leaves out fields that `expected` does not name, so a test checks only what it is about. A difference raises an error that names its path, such as `workspaces[1].windows[1].panels`.
- A Lua error fails the test and logs it with its stack trace. When `run` returns, the program quits without asking about unsaved work.
- The exit status is 0 when the test passes, and not 0 when it fails or cannot start.

## 18. Platform notes

- OpenECS supports X11 and Wayland, through SDL3.
- OpenECS runs on x86_64 and aarch64 processors (17.2).
- On Wayland, dragging a panel out of its window still works, because the core keeps receiving pointer events while the button is held (7.2). Only the position of the new window cannot be chosen.
- Popups are positioned relative to their parent window, which Wayland supports.

---

## Glossary

**ABI (application binary interface).** The rules for how compiled code fits together: where each field of a structure sits in memory, how large each type is, how functions receive arguments. Code that disagrees about the ABI breaks, even though no compiler complained.

**Allman style.** A brace style in which each brace is on its own line.

**AltGr.** A key that many keyboard layouts use to type extra characters, such as @ or {.

**API (application programming interface).** The functions and types that one piece of code offers to another.

**Assertion.** A check of a condition that must always hold. If it fails, the program logs where and stops.

**Binding plugin.** A plugin whose job is to offer a library to other plugins.

**Buffer.** A pointer to memory together with its length.

**Callback.** A function passed to other code so that it can be called later.

**Closure (libffi).** A small piece of code that libffi generates at run time. It looks like an ordinary C function, but forwards each call to a handler, which here calls a Lua function.

**Compositor.** On Wayland, the program that draws all windows on screen and decides where they go and which one has focus.

**Coroutine (Lua).** A Lua function that can pause and later go on from where it paused.

**Destructor.** A function that releases an object when it is no longer needed.

**Environment (Lua).** The table of global names that a piece of Lua code sees.

**Frame.** One pass of drawing and presenting.

**Function pointer.** A value that holds the address of a function, so the function can be called through it.

**GPU device.** SDL's object for the graphics card. Textures and other GPU resources belong to a device.

**Handle.** A token that stands for an object owned by someone else, so it can be passed around without exposing its inside.

**Hit testing.** Finding which element is under a point, such as the pointer.

**Input method.** Software that composes characters a keyboard cannot type directly, for languages such as Chinese or Japanese.

**LuaLS (Lua language server).** The program behind Lua support in editors such as VS Code. It reads annotations in comments, such as `---@param`, to give completion, documentation and warnings.

**Main thread.** The thread on which the program starts. OS windows, input and all Lua code run there.

**Metatable (Lua).** A table that defines how a Lua value behaves. For handles, it carries the type name.

**Opaque handle.** A pointer to a structure whose fields are hidden, so only the functions that own it can change it.

**Offscreen renderer.** A 2D renderer that draws into a texture instead of a window.

**Offscreen video driver.** An SDL video driver that draws OS windows into memory instead of onto a display.

**Premultiplied alpha.** A way of storing transparent pixels in which the colour is already multiplied by the opacity. Blending is faster and has no edge artifacts.

**Protected call (Lua).** A way to call Lua code so that an error is caught and returned, instead of jumping out through the caller.

**Registry (Lua).** A table that Lua keeps for C code to store values it needs to keep alive.

**Root.** The top of a layout tree. There is one per OS window.

**Run path.** A directory written into a program where it looks for shared libraries. `$ORIGIN` means the program's own directory.

**Sanitizer.** A compiler option that adds checks for memory errors and undefined behaviour while the program runs.

**Semantic versioning.** Version numbers of the form MAJOR.MINOR.PATCH, where a new major number means an incompatible change.

**shu.h.** A header of basic types (`i32`, `u32`, `usz`, `SHUSlice`, `SHUResult`) and macros, shared by the core and plugins.

**SHUResult.** shu's result code. `SHUResult_Ok` (zero) means success; every other value names an error.

**SHUSlice.** A pointer and a size that describe a piece of memory, passed together.

**SHUWUR.** A `shu.h` attribute macro that makes the compiler warn when a function's result is not used.

**Static analyzer.** A compiler pass that follows the paths through the code and warns about errors it finds, such as a `NULL` pointer that is dereferenced, without running the program.

**Struct (structure).** A group of named fields stored together in memory.

**Submodule (git).** Another repository included at a fixed version inside a repository.

**Symbol.** A name in a compiled file that other code can look up, such as an exported function.

**Texture.** An image stored on the graphics card.

**Topological order.** An order in which every item comes after the items it depends on.

**Userdata (Lua).** A block of memory managed by Lua's garbage collector that holds data from C. Handles are userdata.

**Vsync.** Waiting for the display's refresh before showing a new frame, so drawing matches the screen's rate.

**X11.** The older display system used on Linux.

**XDG Base Directory specification.** The freedesktop.org rules for where Linux programs keep configuration (`~/.config`), data (`~/.local/share`) and state (`~/.local/state`).

**xdg-activation.** A Wayland protocol through which an application asks the compositor to focus one of its windows.
