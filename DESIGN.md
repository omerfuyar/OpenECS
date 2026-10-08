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
| Internal type, function or macro        | `ECSI_` + PascalCase     | `ECSI_Panel`, `ECSI_LayoutTidy`                         |
| Constant and attribute macro            | `OPENECS_` + UPPER_SNAKE | `OPENECS_API_VERSION`, `OPENECS_EXPORT`                 |
| Struct field, parameter, local variable | camelCase                | `stateVersion`, `minWidth`                              |
| Function pointer field                  | PascalCase               | `Create`, `Draw`, `SaveState`                           |
| Output parameter                        | `ret` + name             | `retPanel`, `retTimer`                                  |
| File                                    | PascalCase               | `OpenECS.h`, `Layout.c`                                 |
| File-level state                        | one `static` struct, UPPER_CASE| `LAYOUT`, `RENDERER`                                    |

- `main.c` and Lua files, such as `manifest.lua` and presets, keep lowercase names.
- A type that tells variants apart ends with `Type`, never `Kind`: `ECSSurfaceType`, `ECSEventType`. Its field is named `type`.
- Lua names use snake_case: `ecs.panel.register_type`, `save_state`. The core's Lua names live under the global table `ecs`. The core's own settings, events and bindable functions start with `ecs.`, for example the setting `ecs.focus`.

### 1.3 Types and results

- The core uses `shu.h` for its basic types (`i32`, `u32`, `f32`, `usz` and so on), for `SHUSlice` and `SHUSliceView`, for `SHUResult` and `SHUWUR`, and for `SHU_ReturnResult`.
- The plugin header includes `shu.h`, so plugins use the same types.
- Memory is passed as a `SHUSlice` or `SHUSliceView`, not as a separate pointer and size.
- An object that plugins hold is an opaque handle: `typedef struct ECSI_Panel *ECSPanel;`. Its fields stay internal.
- A function that creates an object returns `SHUResult` and writes the new handle to an output parameter, which comes first after the plugin: `SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, ...)`. Its `Destroy` or `Stop` takes a pointer to the handle and sets it to `NULL`.
- Other functions take the object they act on first. Functions that act for a plugin take the plugin first.
- Reading and changing a value use `Get` and `Set`: `ECSPanel_GetTitle`, `ECSPanel_SetTitle`.

### 1.4 Errors

- **Recoverable errors** (a missing file, a failed allocation, a bad signature string) return a `SHUResult`. `SHUResult_Ok` is zero, so `if (ECSService_GetFunction(...))` checks for failure. Functions whose result must be checked are marked `SHUWUR`.
- **Contract violations** (a `NULL` handle, an index out of range, a call from the wrong thread) are bugs. They fail an `SDL_assert`. Release builds turn these assertions off.
- Lua never reaches an assertion: the core checks every argument that comes from Lua and raises a Lua error instead (10.8).

### 1.5 Files

- Headers are in `include/`, source files in `src/`.
- `include/OpenECS.h` is the one header that plugins include. It holds every public type and function, with documentation. The build copies only this header to the build's `include/` folder, and plugins are built against that copy, so they never see the core's headers.
- Each module (2.1) is a pair of files: `include/<Module>.h` with the module's declarations and their documentation, and `src/<Module>.c` with the definitions. `src/main.c` holds start-up, the main loop and shutdown.
- A module includes only the modules listed before it in 2.1, so modules never depend on each other in a cycle.
- Headers hold declarations only: types, function declarations and macros. Function and variable definitions, including `static inline` functions, are in source files.
- Headers start with `#pragma once` and group their contents with `#pragma region`. A source file keeps its internal elements in a `Source Only` region.
- Functions used by only one source file are `static`.
- First-party plugins are in `plugins/<name>/`, and first-party presets in `presets/`.

### 1.6 Style

- Documentation comments use `///` with `@brief`, `@param`, `@return` and `@note`, on every public declaration. Each `@return` names the `SHUResult` values the function can return.
- Allman braces and four spaces for indentation.
- An unused parameter is marked `(void)name;`.
- Plain comments are short and lowercase; `// todo` marks unfinished work and `//!` marks something important.
- Compiler attributes are used through macros, never written out: `OPENECS_EXPORT`, `OPENECS_PRINTF`, `SHUWUR`.

### 1.7 Dependencies

- Before writing a function, check that no dependency already has it.
- The core calls SDL directly, without wrappers, for memory, text, files and paths, shared libraries, logging and assertions: `SDL_malloc`, `SDL_strdup`, `SDL_asprintf`, `SDL_GetPrefPath`, `SDL_LoadObject`, `SDL_Log`, `SDL_assert`.
- Dynamic arrays and hash maps come from `stb_ds.h`. The core includes it through the glue header `stb/stbSDL3.h`, which sets it to use SDL's allocator and assertions. The core has no fixed limits on counts, such as the number of plugins or panels.
- Each job has one implementation, used everywhere. For example, all logging goes through SDL's log, and all memory comes from SDL's allocator.

## 2. Program structure

### 2.1 Modules

In order: a module includes only the modules above it (1.5).

| Module   | Job                                                                                                                 |
| -------- | ------------------------------------------------------------------------------------------------------------------- |
| Values   | Generic values (10.3).                                                                                              |
| Lua      | The Lua state. Reads data files (11.2) and runs all Lua code in protected calls.                                    |
| Plugins  | Finding, ordering and loading plugins. Logging for plugins.                                                         |
| Settings | Declarations, layers, explanations.                                                                                 |
| Events   | Core and plugin events, the event queue, timers.                                                                    |
| Services | Function registry, signatures, calls between C and Lua. The only module that calls libffi.                          |
| Panels   | Panel types, panels, and the pixels each panel draws.                                                               |
| Layout   | OS windows and their renderers, layout trees, workspaces, hit testing, docking, the core's own interface. The only module that calls Clay. |
| Input    | SDL's input events, focus, pointer routing, key dispatch, text input.                                               |
| Session  | Reading presets and sessions, applying them, writing them.                                                          |
| Bindings | The `ecs` table: the plugin interface for Lua plugins (11.3).                                                       |

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
9. Enter the main loop.

### 2.4 Shutdown

1. Ask about unsaved work (4.5). The user may cancel.
2. Save the session for the next start (13.4).
3. Destroy the panels. Call each plugin's `Shutdown` in reverse load order.
4. Stop SDL and Lua.

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

The wait uses `SDL_WaitEventTimeout` with the time until the next timer, and does not wait while events are queued. Continuous drawing is paced by the display's refresh (vsync).

### 3.2 Timers

- Plugins ask for timers: once or repeating, with an interval. Timers run on the main thread, in step 3 of the loop.
- `ECSTimer_Start(plugin, &timer, 0.1, true, function, data)` writes a new timer into `timer`; `ECSTimer_Stop(&timer)` stops it. Lua: `ecs.timer.start(0.1, true, fn)`.
- `ECSPanel_StartTimer(panel, &timer, ...)` starts a timer that belongs to a panel. It stops when the panel closes, and its handle is invalid then.
- A one-shot timer ends after its function returns, and its handle is invalid then. A repeating timer that falls behind skips the calls it missed.

### 3.3 Threads

- OS windows, input, layout, event delivery, timers, every call into Lua and every callback that a plugin registers run on the main thread. SDL expects window and event calls there, and a Lua state must not be used by two threads at once.
- Background work posts its result to the main thread, which delivers it.
- `ECS_RunInBackground(plugin, work, done, data)`: `work` runs on a worker thread from a small pool, then `done` runs on the main thread. Lua code never runs on worker threads; Lua plugins use services that do their work in the background.
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
    SHUResult (*Create)(ECSPanel panel, const ECSValue *savedState, u32 version, void **retState);
    void (*Destroy)(void *state);

    // optional, NULL if unused
    void (*Draw)(void *state, ECSSurface *surface, f64 seconds);
    void (*Event)(void *state, const ECSEvent *event);
    SHUResult (*SaveState)(void *state, ECSValue *retState);
    SHUResult (*Save)(void *state); // saves unsaved work
} ECSPanelTypeDesc;
```

- A Lua panel type is a table with the same fields, in Lua's naming: `create`, `draw`, `save_state` and so on.
- `Draw` receives the time in seconds since the panel was last drawn, for animation.
- `Event` returns nothing, because events never ask for permission (8.1).
- Registration: `ECSPanelType_Register(plugin, &desc)`, and in Lua `ecs.panel.register_type(desc)`.

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

- A panel sets its flag with `ECSPanel_SetUnsaved`. Save calls the type's `Save`; if that fails, the close is cancelled.
- The question is a message dialog (`SDL_ShowMessageBox`). When several panels have unsaved work, one dialog lists them all.

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

- A grip appears when the pointer is within a few pixels of a panel's top edge. While it is shown, pointer events over it go to the core.
- Locked groups show no grip and accept no dropped panels.

### 6.5 Operations

- Besides the operations in OVERVIEW 6.3, there are cycling tabs and reopening the last closed panel.
- Illustrative: `ECSLayout_Move(panel, target, ECSZone_Left)` and `ecs.layout.move(panel_id, target_id, "left")`.

### 6.6 Placement of new panels

- The caller can name a group, a side of a panel (split), or a new OS window.

### 6.7 Drop zones

While a panel is dragged, the zones are checked in this order:

1. Outside every OS window: pop out.
2. Within 16 layout units of an OS window's edge: dock along that whole edge.
3. Over a tab row: insert between tabs, at the nearest gap between tab midpoints.
4. In the outer quarter of a panel, at most 80 layout units deep: split toward that side.
5. Anywhere else over a panel: group with it.

On release, the matching operation is called. In small panels, the edge bands shrink so that the centre stays at least a third of the panel.

- A press on a tab or a grip starts a drag once the pointer moves 6 layout units. Escape cancels the drag.
- The drop place is highlighted while the panel is dragged.
- A panel that splits another takes half of the other's place. A panel docked along an edge of the OS window takes a quarter of it.

### 6.8 Clay

- The core computes the rectangles of the layout tree itself.
- Clay lays out the core's own interface on top of them: tab rows, grips and the list of prefix keys. There is one Clay context per OS window (`Clay_SetCurrentContext`).

### 6.9 Maximize, pop-out and workspaces

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
- The keys after the prefix are the setting `ecs.prefix_keys`. Presets and the user can add entries that run service functions.
- The prefix and the key after it are the only key sequence the core handles.
- The default `ecs.prefix_keys`:

  | Key          | Action                 |
  | ------------ | ---------------------- |
  | Arrows       | Move focus             |
  | Shift+Arrows | Move the focused panel into the neighbouring group, or along that edge of the OS window if there is none |
  | 1 to 9       | Switch workspace       |
  | Tab          | Show the next tab      |
  | M            | Maximize or restore    |
  | P            | Pop out                |
  | X            | Close the panel        |
  | Escape       | Cancel                 |

### 7.6 Binding keys

- Keybindings are settings of type `key`.
- Plugins have no function for workspace or global bindings.
- `ECSKey_Bind(plugin, panelType, settingName, function)`: the setting holds the key; the plugin supplies the function.
- A function bound by name takes no arguments, or one argument: the focused panel.
- The core registers its own bindable actions as functions under `ecs`, for example `ecs.focus_left` and `ecs.maximize`. So settings name them like any plugin function.

## 8. Events

### 8.1 Types

- Core events: panel opened, closed, resized, scale changed, shown, hidden, focused, unfocused, moved, popped out, grouped, maximized; workspace switched. Input events (key, pointer, text, drag and drop) go to the panel concerned.
- Plugin events carry a value (10.3).
- Events are notifications. Handlers return nothing and cannot cancel anything.

### 8.2 Delivery

- Events are queued and delivered on the main thread after the current callback returns, never inside another event handler.
- Changes take effect immediately; only the notification waits. A closed panel leaves the layout at once, but its `Destroy` runs after the current delivery finishes, so no handler meets a destroyed panel.

### 8.3 Who receives what

- A plugin may subscribe to another plugin's events only if its manifest depends on that plugin.
- Illustrative: `ECSEvent_Declare(plugin, "canvas.selection_changed", description)`, `ECSEvent_Emit(plugin, name, value)`, `ECSEvent_Subscribe(plugin, name, fn)`. In Lua: `ecs.event.declare`, `emit` and `subscribe`.

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

- All Lua plugins share one Lua state. Each plugin's code runs in its own environment, which contains the `ecs` table.
- The core knows which plugin made an `ecs` call from the environment it came from.

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

Illustrative:

```c
ECSService_RegisterFunction(plugin, "audio.play", AudioPlay, "int(string, float)", "Play a sound file");
```

```lua
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
| `buffer`          | a `SHUSlice`                                       |
| `handle<name>`    | a typed handle (10.6)                              |
| `value`           | a generic value (10.3)                             |
| `fn<signature>`   | a function to call back                            |
| `out <type>`      | an output parameter; in Lua, an extra return value |

- Structures are passed as handles or buffers, never by value.
- There is no fixed limit on the number of parameters.

### 10.3 Values

A generic value (`ECSValue` in C) is nil, a boolean, an integer, a number, a string, a buffer, a handle, or a table (a list or named fields). Saved state, settings, plugin events and generic calls use values.

- The core owns every value. A plugin fills a value that the core gives it, with `ECSValue_SetInteger`, `ECSValue_SetField` and so on.
- Getters take a fallback, returned when the value has another type or is missing: `ECSValue_GetInteger(ECSValue_GetField(state, "document"), 0)`. So saved state from an older or edited file never needs extra checks.
- A table is a list and named fields together. From Lua, integer keys from 1 up to the first missing one are the list, and text keys are the fields. Other keys are reported and skipped.

### 10.4 Calls

- **Lua calls C:** the core makes one Lua function for each registered C function. It converts the arguments by the signature, calls the C function through libffi, and converts the results back.
- **C calls C:** the caller gets the raw function pointer and calls it at full speed.
- **C calls Lua:** the caller also gets a typed C function pointer: a libffi closure that converts the arguments, calls the Lua function in a protected call, and converts the result.
- **Lua calls Lua:** a plain Lua call, because all plugins share one Lua state.
- Callbacks (`fn<...>` parameters) work the same way in both directions.

### 10.5 Lookup

- A plugin asks for a function by name and states the signature it expects. The core compares it with the registered signature and refuses a mismatch, so a version mismatch shows up at lookup instead of crashing a call.
- A plugin may look up only functions of plugins named in its manifest's dependencies. If a provider fails, its users are told.
- Illustrative: `ECSService_GetFunction(plugin, &play, "audio.play", "int(string, float)")`.

### 10.6 Handles

- A handle stands for an object owned by its provider: a pointer plus a type name and a destructor, registered with `ECSHandle_RegisterType`.
- In Lua, a handle is a userdata whose metatable names its type. A handle of the wrong type is rejected with a clear error. When Lua no longer uses a handle, its garbage collector calls the destructor.
- When a provider goes away, its handles become invalid and their users are told.

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

### 11.3 The `ecs` table

| Table                         | Contents                            |
| ----------------------------- | ----------------------------------- |
| `ecs.panel`                   | panel types and panel functions     |
| `ecs.layout`                  | layout operations                   |
| `ecs.workspace`               | workspaces                          |
| `ecs.input`                   | key bindings and focus              |
| `ecs.event`                   | declare, emit and subscribe         |
| `ecs.timer`                   | timers                              |
| `ecs.service`                 | register and look up functions      |
| `ecs.settings`                | declare, get, set, list and explain |
| `ecs.session`                 | save and load                       |
| `ecs.plugin`                  | information about plugins           |
| `ecs.clipboard`, `ecs.dialog` | clipboard and dialogs               |
| `ecs.log`                     | logging                             |

### 11.4 Parity

A list of every public C function with its Lua counterpart is kept and checked by the build or the tests.

### 11.5 Keeping Lua values

Lua functions and values that C code keeps are stored in Lua's registry and referenced by a number. They are released when their owner goes away.

## 12. Settings

### 12.1 Declaring

- The core and plugins declare settings with a name, a type, a default and a description: `ECSSetting_Declare(plugin, &desc)`. Owners are told when their settings change.
- Types: `bool`, `integer`, `number`, `string`, `choice` (one of a list), `key` (a key combination), `list` and `table`.
- `ECSSetting_Get(name)` returns the value in effect as a value (10.3). It comes from the highest layer that sets the setting with a value of its type; otherwise it is the default. A value of another type is reported with its file and skipped.
- The core reads a key combination when it uses it. A key text that cannot be read is reported, and the default is used.

### 12.2 Interface

- `get(name)`, `set(name, value)` (writes the settings window's file), `list()` (every declared setting) and `explain(name)`: the value in effect, the layer it came from, and what each layer says.
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
  plugins_dir = "plugins",                   -- optional extra plugin directory
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
- A panel's saved state is its `state` field, and the state's version is `state_version`.
- Sessions also store each panel's `id`, each workspace's `focus` (a panel id), each group's `shown` panel and `maximized` mark, the `current_workspace`, and `plugin_state`, the state of each plugin.
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
openecs [--preset NAME|FILE] [--session FILE] [--fresh] [FILE...]
```

- `--fresh` starts from the preset instead of the tool's last session.
- A session's identity wins over the preset's, so the session is saved again as the last session of its own tool.
- Files are passed to the function that the preset names in `open`.
- A tool's `.desktop` file runs, for example, `openecs --preset paint %F`.

## 14. Errors and logging

### 14.1 Error reports

- A native plugin that crashes takes the program down. This cannot be prevented and is accepted.
- An error report holds the plugin, the type of error, a message and, for Lua, a stack trace. It goes to the log. The user sees it where it applies: a faulted panel shows it; plugin loading problems appear in the launcher and the log. A message dialog is used only when start-up fails.

### 14.2 Policies

| Situation                                                                             | What happens                                                                                                                                       |
| ------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Manifest unreadable or invalid                                                        | The plugin is skipped and reported, and so are its dependents.                                                                                     |
| Missing dependency or version mismatch                                                | The plugin and its dependents are skipped and reported.                                                                                            |
| Dependency cycle                                                                      | The plugins in the cycle are skipped and reported.                                                                                                 |
| Plugin API version mismatch                                                           | The plugin is refused before any of its code runs.                                                                                                 |
| `ECSPlugin_Init` fails                                                                | The plugin is marked failed and its registrations are removed.                                                                                     |
| Invalid registration (bad signature, bad or duplicate name, the core prefix as a key) | That registration is rejected and the error is returned to the plugin, which continues.                                                            |
| A panel callback raises an error                                                      | The panel becomes a faulted placeholder that shows the error. Its menu has a "Restart" entry, which recreates the panel from its last saved state. |
| A plugin callback (timer, event handler) raises an error                              | The error is reported. Repeats of the same error are counted, not reported again.                                                                  |
| Problems while restoring a session                                                    | See 13.3.                                                                                                                                          |

### 14.3 Error convention

- C errors follow 1.4; Lua errors follow 10.8.
- The core logs the details of each error, so a result code needs no message.

### 14.4 Logging

- Levels: error, warning, info and debug.
- All logging goes through SDL's log, including SDL's own messages. The core calls it directly; plugins call `ECS_Log`, which adds the plugin's name.
- Each line holds the time, the level, the plugin and the message.
- Lines go to standard error and to the log file (16).

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
- The executable exports the functions marked `OPENECS_EXPORT` and nothing else. The core is compiled with hidden symbols by default.
- Native plugins are shared libraries, built against the plugin header only. They do not link against the core; their calls to it are resolved when they are loaded.
- A first-party plugin is built from every C file in its folder, `plugins/<name>/`, so the build needs no settings for each plugin.

### 17.3 Compiler

- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Debug builds add `-fsanitize=address,undefined`.

### 17.4 Dependency versions

Every dependency is a git submodule pinned to a release tag, not to a development commit.

## 18. Platform notes

- OpenECS supports X11 and Wayland, through SDL3.
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

**Destructor.** A function that releases an object when it is no longer needed.

**Environment (Lua).** The table of global names that a piece of Lua code sees.

**Frame.** One pass of drawing and presenting.

**Function pointer.** A value that holds the address of a function, so the function can be called through it.

**GPU device.** SDL's object for the graphics card. Textures and other GPU resources belong to a device.

**Handle.** A token that stands for an object owned by someone else, so it can be passed around without exposing its inside.

**Hit testing.** Finding which element is under a point, such as the pointer.

**Input method.** Software that composes characters a keyboard cannot type directly, for languages such as Chinese or Japanese.

**Main thread.** The thread on which the program starts. OS windows, input and all Lua code run there.

**Metatable (Lua).** A table that defines how a Lua value behaves. For handles, it carries the type name.

**Opaque handle.** A pointer to a structure whose fields are hidden, so only the functions that own it can change it.

**Offscreen renderer.** A 2D renderer that draws into a texture instead of a window.

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
