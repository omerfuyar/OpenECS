# OpenECS Design

This document explains what OpenECS is and how it is built: its scope, its principles, how it behaves to the user, and its modules, interfaces, data, rules and dependencies.

**How to read it:**

- Everything here is decided. Open questions and pending work are in [TODO.md](TODO.md).
- How to use a tool made with OpenECS is in [GUIDE.md](GUIDE.md).
- Each section starts with what it means for the user and for plugins, then how it is built.
- Code samples follow the conventions in section 1, but are only illustrations of an interface.
- Terms are explained in the glossary at the end.

---

## Contents

0. What OpenECS is
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
19. Versions and releases
20. Standard plugins

- Glossary

---

## 0. What OpenECS is

### 0.1 Summary

OpenECS is a program, written in C, that provides the domain agnostic features of an editor style application: its front end. Blender, Unity, DaVinci Resolve and VS Code each have one, with tabs, panels, pop-out windows and workspaces. OpenECS provides all of these.

What each area shows and does comes from **plugins**: add-on modules written in C or in Lua. So the same program becomes a text editor, a paint program, the front end of a game engine, or a game, depending on which plugins and **preset** it loads. Used that way, OpenECS is a cross-platform application platform.

### 0.2 Core and plugins

The most important rule of OpenECS is the line between the **core** (OpenECS itself) and **plugins** (everything else). The core is small and domain agnostic: it provides the machinery of applications. If the core's own machinery needs something, it is core; if only some panels need it, it is a plugin.

The core provides:

- Layout: panels, splits, groups, maximizing, pop-out windows and workspaces.
- A drawing surface for every panel, and putting it on screen. The core offers **no drawing commands**: nothing like "draw a rectangle" or "draw text".
- Its own small interface: the windows' title bars, tab rows, dividers, drag highlights and menus.
- Input: focus, keybindings, the mouse pointer and text input.
- Events and actions for everything about panels and layout.
- Plugin loading, and the services and events through which plugins work together.
- Presets, sessions and settings.
- Operating-system features, such as popups, the clipboard, drag and drop, and dialogs (4.6, 18).
- All of the above from Lua too.

Plugins provide everything else, for example:

- Drawing shapes, text and images, and user-interface elements such as buttons and lists.
- Loading images, 3D models, audio and other media.
- Text editing, painting and all other tool logic.
- Audio, networking and 3D rendering.
- Even the core's own windows, such as the settings window (20).

### 0.3 Platform

OpenECS runs on Linux, its priority, and on Windows. Other platforms are not worked on for now (19.4).

What depends on the platform stays behind an interface, so porting changes as little as it can: the core reaches the system through SDL, and a plugin that uses the system gives other plugins functions that do not, such as fs for folders and net for sockets. Plugins and tools built on those interfaces keep working when OpenECS or a plugin is ported.

### 0.4 Not priorities, and not in the design

- **Security.** There is no permission system and no protection against malicious plugins or files.
- **Accessibility.** The application is not designed with accessibility in mind.
- **Look and feel.** A theme is a set of core settings (colours, font, sizes) that the core's interface uses and plugins can read (12.4). OpenECS ships a dark and a light theme, and follows the system's choice by default.
- **Undo and redo.** Each panel or plugin keeps its own history and binds its own keys; the core keeps no global history.
- **Not in the design:** automatic saving while OpenECS runs, crash recovery, and a library version of OpenECS.

### 0.5 Architecture

``` text
+--------------------------------------------------------------+
|  Plugins: native (C) and Lua                                 |
+--------------------------------------------------------------+
|  Plugin API: the only way plugins talk to the core           |
+--------------------------------------------------------------+
|  Core logic: layout, workspaces, input, events, services,    |
|  settings, presets and sessions, plugin loading              |
+--------------------------------------------------------------+
|  Libraries: SDL3, SDL3_ttf, Clay, LuaJIT, libffi             |
+--------------------------------------------------------------+
```

Plugins talk only to the plugin API (2.2). They never receive an SDL, Lua, Clay or libffi object from it; a native plugin that needs the GPU device gets it from `ECSGpu_GetDevice` (9.8).

### 0.6 Principles

1. **Shallow core.** The core provides mechanisms. Plugins and settings decide what is shown and what keys do. Nothing domain-specific goes into the core.
2. **Everything is available from Lua.** Lua can do everything that C can.
3. **Plain functions.** Actions are plain functions that code calls directly. Settings and presets refer to them by their service name. There is no separate command system.
4. **Calls are written out.** Code says what it calls and when. No function registers or calls other functions unseen: a panel's code calls what draws it, hands its events on and frees its state, each in its own line.
5. **One front door.** Plugins reach the core only through the plugin API. The core checks its rules when something is used, not only when it is registered.
6. **Small, uniform API.** Few concepts, used the same way everywhere.
7. **Hide dependencies.** Plugins never get SDL, Lua, Clay or libffi from the API, so these can change without breaking plugins.
8. **Least authority.** A plugin can do only what its job needs (9). Presets and the user can do more, because they assemble the tool.
9. **The user decides.** The user's settings and keybindings win, the core's keys are always reachable, and no plugin can block closing.
10. **Panels are views.** Shared data lives in services, not inside panels.
11. **Nothing runs without a reason.** Code runs because of input, timers, visible animation or finished background work, never by polling.
12. **Do not lose what is not understood.** Unknown settings, missing panel types and unreadable state are kept.
13. **Errors stay local.** A broken script or panel affects only itself.
14. **The core's own tools are plugins.** Rich windows, such as the settings window, are built on the plugin API, which proves that the API is complete.
15. **Plugins stand alone.** A plugin uses only the libraries it brings, compiled into itself, and never another plugin's. The one exception is the platform the executable loads, SDL3 and SDL3_ttf, which the standard plugins call as they call the core (17.2). Plugins depend on each other only through the services their manifests name in `depends`, so any plugin that no other one names can be left out of a tool on its own (20.1).
16. **Written for Lua.** Lua is where tools are read and written most, so plugins' functions read well there: functions of one kind share a namespace, such as `parse.json.read`, and a handle's or a struct's functions are its methods, so `a + b` and `connection:send(data)` read as Lua does. An object with a lifetime is a handle, and plain data, such as a vector, is a struct: a table of its fields. C calls the same functions by their full names.
17. **No exceptions.** A rule holds everywhere it applies. When a case does not fit a rule, the rule or the case changes, so that it fits; the design does not keep an exception for it.

## 1. Conventions

### 1.1 Language

- C23 standard is used everywhere. Its forms are preferred to older ones: `nullptr` instead of `NULL`, `static_assert`, `constexpr`, `bool` as a keyword, and enumerations with a fixed type.
- Lua is LuaJIT 2.1, with its extensions (9.6). Lua code writes LuaJIT's syntax where it is shorter or safer:
  - `+=`, `-=`, `*=` and `..=` for a value that changes by itself: `count += 1`, `text ..= line`;
  - `continue` to skip the rest of a loop's pass, in place of nesting the rest in an `if`;
  - `??` for a default that may be `false`, as `or` replaces `false` too: `props.visible ?? true`;
  - `?.` to read through a value that may be nil: `state.selected?.name`;
  - the bit operators `& | ~ << >>` in place of the `bit` library; they work on 32 bits and give signed results, so `0xFF000000 | 0` is negative (11.7);
  - `const` for a local that never changes, such as a table of constants at a file's top: `const SIZES = { ... }`.
- A whole division is `math.floor(a / b)`: LuaJIT has no `//`.
- A Lua plugin's panel type is a class: a table, such as `Canvas`, with `Canvas.__index = Canvas`, whose functions are methods. Its `create` gives the panel's state as an object of the class, `setmetatable({ panel = panel }, Canvas)`, and the type's description names the methods: `draw = Canvas.draw`. The core calls `draw(state, surface, seconds)`, which is `state:draw(surface, seconds)`, so each method takes the state as `self`. The plugin's own functions on a panel are methods too: `self:load(path)`.

  ```lua
  local Canvas = {}
  Canvas.__index = Canvas

  function Canvas.create(panel, saved, version)
    return setmetatable({ panel = panel, strokes = saved or {} }, Canvas)
  end

  function Canvas:draw(surface, seconds)
    surface:fill(0, 0, surface.width, surface.height, "background")
  end

  ecs.panel.registerType({ name = "canvas", title = "Canvas", create = Canvas.create, draw = Canvas.draw, destroy = Canvas.destroy })
  ```

- A class gives methods, and operators where its values are numbers of a kind, as structs' are (10.11). An `__index` that is a function, or a `__newindex`, does work out of sight and breaks principle 4 (0.6): a setting is read with `ecs.setting.get`, not as a field.
- Bytes for C, such as vertices, pixels and samples, are built in an FFI array and given as a string: `ffi.new("float[?]", count)`, then `ffi.string(array, ffi.sizeof(array))`. A string's bytes are read through a pointer: `ffi.cast("const float *", text)`, valid while the string is.

### 1.2 Names

| What                                     | Form                            | Example                                                           |
| ---------------------------------------- | ------------------------------- | ----------------------------------------------------------------- |
| Public type                              | `ECS` + PascalCase              | `ECSPanel`, `ECSSurface`, `ECSPanelTypeDesc`                      |
| Function that belongs to a type          | `ECS<Type>_<Verb>`              | `ECSPanel_SetTitle`, `ECSLayout_Move`, `ECSTimer_Start`           |
| Function that belongs to no type         | `ECS_<Verb>`                    | `ECS_RunInBackground`, `ECS_Log`                                  |
| Enumeration value                        | `<Type>_<Value>`                | `ECSSurfaceType_Gpu`, `ECSZone_Left`                              |
| Internal type                            | `ECSI` + PascalCase             | `ECSIPanel`, `ECSINode`, `ECSIZone`                               |
| Internal function or function-like macro | `ECSI<Type or module>_<Verb>`   | `ECSIValue_Clear`, `ECSILayout_Tidy`, `ECSILua_Call`              |
| Constant: macro or constant table        | `OPENECS_` + UPPER_SNAKE        | `OPENECS_API_VERSION`, `OPENECS_EXPORT`, `OPENECS_CORE_FUNCTIONS` |
| Struct field, parameter, local variable  | camelCase                       | `stateVersion`, `minWidth`                                        |
| Function pointer field                   | PascalCase                      | `Create`, `Draw`, `SaveState`                                     |
| Function pointer type                    | `ECS` + PascalCase + `Function` | `ECSTimerFunction`, `ECSPanelDrawFunction`                        |
| Output parameter                         | `ret` + name                    | `retPanel`, `retTimer`                                            |
| File                                     | PascalCase                      | `OpenECS.h`, `Layout.c`                                           |
| File-level state                         | one `static` struct, UPPER_CASE | `LAYOUT`, `RENDERER`                                              |

- Internal names follow the public ones with `ECSI` in place of `ECS`. An internal enumeration value is `<Type>_<Value>` too: `ECSINodeType_Split`.
- `main.c` and Lua files, such as `manifest.lua` and presets, keep lowercase names.
- A type that tells variants apart ends with `Type`, never `Kind`: `ECSSurfaceType`, `ECSPanelEventType`. Its field is named `type`.
- Lua names use camelCase: `ecs.panel.registerType`, `saveState`, the setting `ecs.keepSession` and the event type `pointerDown`. The core's Lua names live in the `ecs` module (9.6). The core's own settings, events and bindable functions start with `ecs.`, for example the setting `ecs.focus`. A plugin declares its names without its own name before them (9.9).

### 1.3 Types and results

- The core uses `shu.h` for its basic types (`i32`, `u32`, `f32`, `usz` and so on), for `SHUSlice` and `SHUSliceView`, for `SHUResult` and `SHUWUR`, and for `SHU_ReturnResult`.
- The plugin header includes `shu.h`, so plugins use the same types.
- Memory is passed as a `SHUSlice` or `SHUSliceView`, not as a separate pointer and size.
- Every function pointer type has a typedef, which fields, parameters and casts use.
- An object that plugins hold is an opaque handle: `typedef struct ECSIPanel *ECSPanel;`. Its fields stay internal.
- A function that creates an object returns `SHUResult` and writes the new handle to an output parameter, which comes first after the plugin: `SHUResult ECSTimer_Start(ECSPlugin plugin, ECSTimer *retTimer, ...)`. Its `Destroy` or `Stop` takes a pointer to the handle and sets it to `nullptr`.
- Other functions take the object they act on first. Functions that act for a plugin take the plugin first.
- Reading and changing a value use `Get` and `Set`: `ECSPanel_GetTitle`, `ECSPanel_SetTitle`. In Lua, such a value is a field of the handle instead: `panel.title` (11.4).

### 1.4 Errors

Every function, of the core or of a plugin, in C or in Lua, fails in one of two ways.

- **A failure** has a cause outside the caller: a missing file, a lost connection, a shader that the GPU refuses, memory that runs out. The function gives nothing and why, and the caller decides what to do.
  - A core function in C returns a `SHUResult`. `SHUResult_Ok` is zero, so `if (ECSService_GetFunction(...))` checks for failure. Functions whose result must be checked are marked `SHUWUR`. The core logs why (14.3).
  - A core function in Lua gives `nil, message`, except when memory runs out: that raises an error, as Lua's own allocations do.
  - A service gives why as its last result, `out string error` (10.8).
- **A misuse** is a bug of the caller: an argument of the wrong type, a `nullptr` handle, an index out of range, data that does not fit, such as too few pixels for an image's size, a call from the wrong thread, or a registration that is not valid, such as a bad signature or a second one of a name. Each function checks what it is given with `ECS_Check` in C and `ecs.check` in Lua, in every build. A misused call never finishes as asked, and the callback that made it fails (14.2).
  - Lua code stops at the call: it raises an error at the line that made it.
  - C code cannot be stopped in the middle, so the call returns and gives nothing: `nullptr`, zero, or `SHUResult_ErrAssertion` from a function that returns a `SHUResult`. The code runs on to the end of its callback.
- `SDL_assert` checks only the core's own invariants, which hold whatever plugins do; a failed one is a bug of OpenECS. Release builds turn these assertions off.

### 1.5 Files

- Headers are in `include/`, source files in `src/`.
- `include/OpenECS.h` is the one header that plugins include, with `shu.h`, which it includes. It holds every public type and function, with documentation. The build copies these two and `include/ecs.lua` (9.6) to the build's `include/` folder, beside the plugins' definition files (10.10), and plugins are built against that folder alone, so they never see the core's headers or its dependencies'.
- Each module (2.1) is a pair of files in its group's folder: `include/<group>/<Module>.h` with the module's declarations and their documentation, and `src/<group>/<Module>.c` with the definitions. Includes name the folder: `#include "base/Values.h"`.
- `src/main.c` only reads the command line and runs the App module.
- A module includes only the modules listed before it in 2.1, so modules never depend on each other in a cycle. So a group includes only its own folder and the groups before it.
- A source file includes its own header first, then the core's other headers sorted by path, then the dependencies' headers.
- Headers hold declarations only: types, function declarations and macros. Function and variable definitions, including `static inline` functions, are in source files.
- Headers start with `#pragma once` and group their contents with `#pragma region`. A source file keeps its internal elements in a `Source Only` region.
- Functions used by only one source file are `static`.
- First-party plugins and presets are in OpenECS-std, in `std/` (17.7).
- Examples are in OpenECS-examples, in `examples/` (17.6). They are the tests too (17.5).

### 1.6 Style

- Documentation comments use `///` with `@brief`, `@param`, `@return` and `@note`, on every public declaration. They say what a thing does and what it takes, not which modules or plugins use it, as that changes without the thing changing. Each `@return` names the `SHUResult` values the function can return. A public function's comment ends with `@lua` (11.5).
- Allman braces and four spaces for indentation. clang-format formats the code as `.clang-format` says, and clang-tidy checks names (1.2), the length of names, how hard a function is to follow, and common bugs as `.clang-tidy` says.
- A variable or parameter name has at least three letters; only `i`, `j`, `k`, `x`, `y`, `z`, `w`, `h` and `id` are shorter.
- A function that grows long or nested is split into functions that each do one job and say it in their names.
- An unused parameter is marked `(void)name;`.
- Plain comments are short and lowercase; `// todo` marks unfinished work and `//!` marks something important.
- Compiler attributes are used through macros, never written out: `OPENECS_EXPORT`, `OPENECS_PRINTF`, `SHUWUR`.

### 1.7 Dependencies

- Before writing a function, check that no dependency already has it.
- The core calls SDL directly, without wrappers, for memory, text, files and paths, shared libraries, logging and assertions: `SDL_malloc`, `SDL_strdup`, `SDL_asprintf`, `SDL_GetPrefPath`, `SDL_LoadObject`, `SDL_Log`, `SDL_assert`.
- Dynamic arrays and hash maps come from `stb_ds.h`. The core includes it through the glue header `stb/stbSDL3.h`, which sets it to use SDL's allocator and assertions. The core has no fixed limits on counts, such as the number of plugins or panels.
- Each job has one implementation, used everywhere. For example, all logging goes through SDL's log, and all memory comes from SDL's allocator, except Lua's: LuaJIT's own allocator gives Lua its many small objects.

## 2. Program structure

### 2.1 Modules

In order: a module includes only the modules above it (1.5). The modules are in four groups, each in its own folder.

| Group     | Module     | Job                                                                                                                                                            |
| --------- | ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| base      | Log        | Where SDL's log goes, and how its lines look (14.4).                                                                                                           |
|           | Paths      | What a path looks like on each system, and paths relative to the program's folder or to a file.                                                                |
|           | Sanitizers | The sanitizers' settings in Debug builds (17.3).                                                                                                               |
|           | Values     | Generic values (10.3).                                                                                                                                         |
|           | Lua        | The Lua state. Reads data files (11.2) and runs all Lua code in protected calls.                                                                               |
|           | Timers     | Functions that run on the main thread after a time, once or again and again (3.2).                                                                             |
|           | Profile    | Where the main thread's time goes, by step of the loop, by plugin and by panel (3.5).                                                                          |
| runtime   | Plugins    | Finding, ordering and loading plugins. Logging for plugins.                                                                                                    |
|           | Settings   | Declarations, layers, explanations.                                                                                                                            |
|           | Events     | Core and plugin events, the event queue, the plugins' timers on base/Timers.                                                                                   |
|           | Tasks      | Work on worker threads, and functions that other threads send to the main thread (3.3).                                                                        |
|           | Services   | Function registry, signatures, calls between C and Lua, definition files (10.10). The only module that calls libffi.                                           |
| interface | Panels     | Panel types, panels, and the pixels each panel draws.                                                                                                          |
|           | Layout     | Workspaces and their layout trees: operations, tidying, sizes, focus, closed panels and saving.                                                                |
|           | Popups     | Popups, the pixels each popup draws, and its events (4.6).                                                                                                     |
|           | Window     | OS windows and their renderers, frame pacing, and the core's own interface: tab rows, grips, menus, dragging and hit testing. The only module that calls Clay. |
|           | Clipboard  | Texts and typed data on the system's clipboard.                                                                                                                |
|           | Dialogs    | The system's file dialogs, and the message dialogs that the core draws (7.7).                                                                                  |
|           | Keys       | Key combinations, the core prefix and the keys after it, and every key binding (7.3 to 7.5, 7.8).                                                              |
|           | Menus      | The core's bindable functions, and the menus that offer them (6.8).                                                                                            |
|           | Input      | SDL's input events, focus, pointer routing, key dispatch, the event of the prefix, text input.                                                                 |
| app       | Session    | Reading presets and sessions, applying them, writing them.                                                                                                     |
|           | Desktop    | The tools' entries in the system's application launcher (13.9).                                                                                                |
|           | Workers    | Lua scripts on threads of their own, and the messages between them and the main thread (3.4).                                                                  |
|           | Bindings   | The `ecs` module: the plugin interface for Lua plugins (11.3).                                                                                                 |
|           | Test       | Runs a test (17.5).                                                                                                                                            |
|           | App        | Start-up, the main loop and shutdown (2.3, 3.1, 2.4).                                                                                                          |

### 2.2 The boundary

- No header that plugins include, and no function that plugins call, exposes a type from SDL, Lua, Clay or libffi.
- `shu.h` is the exception; it is part of the plugin interface (1.3).
- Native plugins get the core's SDL objects only through `ECSGpu_GetDevice` and a GPU surface's texture (9.8). The plugin interface includes no dependency's header: a plugin that uses a library, SDL too, brings it itself (17.2). Inside the core, every module calls SDL directly (1.7).
- Plugins can call only the core functions marked `OPENECS_EXPORT`. Every other function of the core is hidden from them (17.2).

### 2.3 Start-up

1. Read the command line (13.6) and find the preset or session.
2. Start Lua. Read the preset's identity, plugins and settings as data (11.2).
3. Give SDL the tool's identity (name, icon, app id) with `SDL_SetAppMetadata`. This happens before any OS window exists, because SDL needs the identity before it starts.
4. Start SDL, and open the main OS window and its renderer.
5. Find plugins, read their manifests, resolve dependencies and compute the load order (9.3).
6. Load each plugin in order and call its `Init`. Plugins register what they provide.
7. Build the settings layers (12).
8. Restore plugin state, then build the layout and panels, from the preset or, with `ecs.keepSession`, from this tool's last session (13.4).
9. Report the settings and keys whose names nothing registered (7.8, 12.1).
10. Pass the files of the command line to the `open` function (13.6).
11. Enter the main loop.

### 2.4 Shutdown

1. Ask about unsaved work (4.5). The user may cancel.
2. Save the session for the next start (13.4).
3. Destroy the panels, then the objects of handles (10.6).
4. Stop the background work (3.3): running work finishes, waiting work is dropped, and their `done` functions run.
5. Destroy the plugins in reverse load order: each plugin's `Destroy` function runs (9.4). Timers, events and the functions of other plugins still work then.
6. Stop the Lua workers (3.4) and wait for their threads. Forget the handles that plugins made while they shut down, and free the services.
7. Free the timers, then unload the plugins' libraries.
8. Stop SDL and Lua.

## 3. Main loop, timers and threads

### 3.1 The loop

The loop wakes up only when an input event arrives, a timer is due, background work finishes, or a frame is due because a visible panel draws continuously.

One pass of the loop:

1. Wait for one of the reasons above.
2. Handle input: hit testing, focus and key dispatch (7).
3. Run the functions that other threads sent to the main thread (3.3), and the timers that are due.
4. Deliver queued events (8).
5. Recompute the layout if it changed.
6. Draw the visible panels that need it: continuous panels, and panels that asked to be redrawn or changed size or scale.
7. Compose and present the OS windows that changed: every visible one after a change that may show anywhere, such as the layout, a setting or the interface's state, and otherwise those whose own panels or popups drew. A popup window is presented when it drew or moved.

The wait uses `SDL_WaitEventTimeout` with the time until the next timer or the next allowed frame, and does not wait while events are queued.

- Continuous drawing is paced by the display's refresh (vsync). The setting `ecs.vsync` is the frame rate in percent of the refresh rate: 100 (the default) waits for every refresh, 50 for every second one, and 0 turns vsync off with no limit. A percentage that divides 100, such as 50, sets SDL's vsync interval; others keep vsync on and the core limits the frames.
- On Wayland, vsync is off and the core limits the frames to the rate that `ecs.vsync` asks for: the compositor shows whole frames already, and it gives a hidden window no refreshes, so waiting for one would stop the program.
- Some drivers accept vsync without waiting for it, so the core also limits frames to 1.1 times the rate that `ecs.vsync` asks for. A working vsync still sets the pace.
- A hidden, minimized or covered window is not drawn. A screenshot of the tests draws every shown window.
- Panels draw before the core declares its interface, because a panel's `Draw` may change its title, which the interface shows.

### 3.2 Timers

- Plugins ask for timers: once or repeating, with an interval. Timers run on the main thread, in step 3 of the loop.
- `ECSTimer_Start(plugin, &timer, 0.1, true, function, data)` writes a new timer into `timer`; `ECSTimer_Stop(&timer)` stops it. Lua: `ecs.timer.start(0.1, true, callback)` returns a handle, and `handle:stop()` stops it.
- Timers belong to the plugin, as subscriptions do. A panel that starts a timer stops it in its type's `Destroy`.
- A one-shot timer ends when its function runs. Stopping a timer that ended or stopped does nothing, so a panel stops its timers in `Destroy` whether they fired or not. A repeating timer that falls behind skips the calls it missed.
- The core keeps timers in a table of slots, with a free list. A handle names a slot and its generation, which grows when the timer ends or stops, so an old handle names nothing.

### 3.3 Threads

- OS windows, input, layout, event delivery, timers, every call into the plugins' Lua state and every callback that a plugin registers run on the main thread. SDL expects window and event calls there, and a Lua state must not be used by two threads at once.
- Background work posts its result to the main thread, which delivers it.
- `ECS_RunInBackground(plugin, work, done, data)`: `work` runs on a worker thread from a small pool, then `done(data, dropped)` runs on the main thread. `done` runs once for each call: `dropped` is true when the work never ran, so `done` is where the plugin frees `data`. A plugin's Lua code never runs on these threads. A Lua plugin uses services that work in the background, or starts a worker (3.4), which runs a script of its own.
- The pool has one thread for each processor core but one, at most 4. It starts when background work first comes.
- A function that another thread sends to the main thread waits in the core's queue, which the loop empties at one step of each pass (3.1); the functions that come meanwhile wait for the next pass. Message dialogs (7.7) and disabling a plugin (9.10) never empty it, so these functions never run inside other code; disabling takes out only the plugin's own `done` functions, and runs them. A function that the main thread sends to itself runs after the current callback returns, with the queued events (8.2).
- On exit, running work finishes and waiting work does not run; its `done` gets `dropped`. Both happen before the plugins' `Destroy` functions run (2.4), so no work uses what `Destroy` freed.
- The thread-safe functions are: running a function on the main thread (`ECS_RunOnMainThread`, built on `SDL_RunOnMainThread`), `ECS_RunInBackground`, and logging. Every other function is for the main thread only. Each function documents its thread rule.
- Native plugins may create their own threads, under the same rule.

### 3.4 Workers

A worker is a Lua script that runs on a thread of its own, in a Lua state of its own. Lua plugins use workers for long work, such as counting or parsing, while the main thread stays free.

- `ecs.worker.start(desc)` starts a worker and gives its handle, or nil and a message. The description holds:
  - `file`, the script, a path in the plugin's folder;
  - `input`, a value that the script gets as its `...`;
  - `message`, a function that gets each value that the script sends;
  - `done`, a function that gets the end of the script: `true` and the value it returned, or `false` and its error.
- `worker:send(value)` sends a value to the worker. `worker:stop()` stops it. `worker.running` is whether its script still runs.
- Inside the script, `require("ecs")` gives a small table: `ecs.log`, `ecs.plugin` (`name`, `version` and `folder`), and the worker's own functions:
  - `ecs.worker.receive(seconds)` gives the oldest value that the plugin sent. It waits for one at most `seconds`, or without end if `seconds` is missing. It gives nil when the time runs out.
  - `ecs.worker.send(value)` sends a value to the plugin.
- The script has every Lua library, as plugins do (9.6). Its `require` finds modules in the plugin's folder first. It cannot call the core or other plugins, because they run on the main thread only.
- Values go between the threads as copies: the bytes that LuaJIT's `string.buffer` encodes them to, decoded on the other side. They are nil, booleans, numbers, LuaJIT's 64-bit integers, texts, and tables of them, without their metatables. A function, a handle, a struct or any other userdata or cdata cannot be encoded, so sending one, starting a worker with one in `input`, or returning one from the script is an error. A struct goes as the table of its fields.
- `message` and `done` run on the main thread, between passes of the loop, like a background task's `done` (3.3). An error in them is reported (14). So is an error in the script, which ends it.
- `stop` ends the script at its next `receive` or call of `ecs`; a loop that calls neither runs until it ends, as LuaJIT runs no hooks in the code it compiles. After `stop`, its `message` and `done` are not called again. A script that waits in a C function, such as `io.read`, ends when that function returns.
- A worker runs until its script ends or it is stopped, even when its handle is dropped. On exit, the core stops every worker and waits for it (2.4).
- Lua only: a native plugin runs work with `ECS_RunInBackground`, or on threads of its own (3.3).

### 3.5 Profile

The core measures where the main thread's time goes while the setting `ecs.profile` is on, so a profiler plugin can show it. Off, it measures nothing and costs nothing.

- **Steps.** Each moment of a pass of the loop belongs to one step: `wait`, `input`, `timers`, `events` (the rest of the pass before the frame), `layout`, `panels`, `upload` (the panels' pixels copied to their textures), `interface` (the core's own) and `present`. The core keeps the steps of the last 240 passes.
- **Plugins.** A plugin's time is the time its code runs on the main thread: its callbacks, and its functions that other plugins call through the core, from Lua to C and from C to Lua. A plugin's time leaves out the plugins it calls, so a Lua panel's calls into draw are draw's. Calls that do not pass the core, from C to C by raw pointer and from Lua to Lua, stay the caller's (10.4). The core's own callbacks and services count as `ecs`. Other threads are not measured, as they run beside the frames.
- **Panels.** A panel's time is its `Draw`, with the plugins it calls.
- **Lua functions.** LuaJIT's sampling profiler looks at the main thread's Lua code every millisecond, and counts the Lua function that runs: compiled, interpreted, or in Lua's garbage collector or LuaJIT's compiler. A sample in C code counts toward no function, as the plugin of that C has the time. A function is named by its file, from its plugin's folder, and by its name, or the line where it starts: `init.lua:element`. Its plugin is the one whose folder holds the file; the core's own Lua code is `ecs`. LuaJIT has one profiler, so a plugin that starts its own, such as `jit.p`, ends this sampling until `ecs.profile` is turned on again.
- Plugins' and panels' times are summed over a second: their time, their longest call and their number of calls. The profile gives the last whole second's. So it does for Lua functions: their samples, and their seconds, the samples' milliseconds.
- `ECSProfile_Get(value)` fills a value with the profile, in Lua `ecs.profile.get()`: `passes`, the last passes' steps, the oldest first; `plugins` and `panels`, the last second's times, the most first; and `functions`, the 50 Lua functions that took the most of the last second, each with its `name`, `plugin`, `samples` and `seconds`. It gives nil while `ecs.profile` is off.

## 4. Panel types and panels

A panel is a box in the layout. The core decides where the box is, how big it is, and whether it is visible or focused. The panel's type decides everything inside the box.

- **When panels run.** OpenECS does nothing unless something happens. Code runs because of input, timers that a plugin asks for, continuous drawing that a panel type asks for while it is visible, and background work that finished on another thread. Any other panel is drawn only when its content or its size changed. Panels in hidden workspaces are not drawn, but their timers keep running. An idle OpenECS uses no processor time.
- **State.** Each panel has its own state. A panel type can save it into the session and load it back, with a version number, so it can read older saves.
- **Unsaved work.** A panel can mark itself as having unsaved work and offer a save function. Plugins never block closing: before a panel with unsaved work closes, the core asks the user to Save, Discard or Cancel, whether the panel closes by itself, because the user quits, or because another session is being loaded.
- **Beyond the panel's box.** Only the core can do what crosses panel borders or belongs to the OS window: popups, such as menus, suggestions or tooltips, which may extend past the panel and the OS window; the pointer's shape and lock, which ends when the panel loses focus; and drag and drop of data between panels (7.9).

### 4.1 Panel type descriptor

```c
/// @brief Describes a panel type. Passed to ECSPanelType_Register.
typedef struct ECSPanelTypeDesc
{
    const char *name;        // the local name, such as "view"; the type is "canvas.view" (9.9)
    const char *title;       // default title for tabs and menus
    u32 stateVersion;        // version of the saved state
    ECSSurfaceType surface;  // ECSSurfaceType_Pixels or ECSSurfaceType_Gpu
    bool continuous;         // draw every frame while visible
    f32 minWidth;            // in layout units, 0 for none
    f32 minHeight;
    const char *const *redrawOn; // names of events that draw every panel of the type again (4.3)
    usz redrawOnCount;

    // required
    ECSPanelCreateFunction Create;       // SHUResult (ECSPanel panel, const ECSValue *savedState, u32 version, void **retState)
    ECSPanelDestroyFunction Destroy;     // void (void *state)

    // optional, NULL if unused
    ECSPanelDrawFunction Draw;           // void (void *state, ECSSurface *surface, f64 seconds)
    ECSPanelEventFunction Event;         // void (void *state, const ECSPanelEvent *event)
    ECSPanelSaveStateFunction SaveState; // SHUResult (void *state, ECSValue *retSavedState)
    ECSPanelSaveFunction Save;           // SHUResult (void *state): saves unsaved work
} ECSPanelTypeDesc;
```

- A Lua panel type is a table with the same fields, in Lua's naming: `create`, `draw`, `saveState` and so on. Its functions are the methods of the plugin's class, and its state an object of it (1.1).
- `Draw` receives the time in seconds since the panel was last drawn, for animation.
- `Event` returns nothing, because events never ask for permission (8.1).
- Registration: `ECSPanelType_Register(plugin, &desc)`, and in Lua `ecs.panel.registerType(desc)`.
- `ECSPanelType_List`, and in Lua `ecs.panel.listTypes()`, list the registered panel types, by plugin and then by title, with the plugin's description, so a plugin can offer to open them (12.6).

### 4.2 Panels

- A panel has a stable id, its type, the state its type created, a title, a size, a scale, a visibility and an unsaved-work flag.
- Lifecycle: `Create` (with saved state when restoring), then `Draw` and `Event` calls, then `Destroy`.
- Functions a panel calls about itself (illustrative): `ECSPanel_Redraw`, `ECSPanel_SetTitle`, `ECSPanel_SetUnsaved`, `ECSPanel_SetCursor`, `ECSPanel_LockPointer`, `ECSPanel_SetTextInput`, `ECSPanel_AcceptDrops`, `ECSPopup_Open`.

### 4.3 When a panel is drawn

- A panel that is not continuous asks to be drawn with `ECSPanel_Redraw`. The core also draws it when its size or scale changes, and draws every panel and popup again, whole, when a setting changes, as any setting may change how they look, such as a colour of the theme.
- A panel type names events in `redrawOn`, such as `{ "files.changed" }`: each time one of them is delivered, after its subscribers, the core draws every panel of the type again, as `ECSPanel_Redraw` asks, and a hidden one when it is next shown. In Lua it is a list: `redrawOn = { "files.changed" }`. So a plugin whose panels show data that an event says changed keeps no list of its panels.
  - The name is matched when the event is delivered, so the event may be declared after the type; a name that nothing declares draws nothing and is no error.
  - The type's plugin hears only what a subscription hears (8.3): the core's events, its own, and those of the plugins its manifest depends on. A name that is declared when the type is registered, by a plugin it does not depend on, is a misuse; one declared later by such a plugin draws nothing.
  - A reloaded plugin (9.11) registers its types again, with the names that its new code gives. A reloaded dependency declares its events again, and the names match them as before.
- A panel in a hidden workspace runs only when its timers or events call it.

### 4.4 Placeholders

- A panel whose type is missing is a placeholder that keeps its saved state (13.3).
- A panel whose callback raised an error becomes a faulted placeholder (14.2).

### 4.5 Unsaved work

- A panel sets its flag with `ECSPanel_SetUnsaved`, and a Lua panel with `panel.unsaved = true`. Its tab shows a mark. Save calls the type's `Save`; if that fails, the close is cancelled.
- The question is a message dialog (7.7) with Save, Discard and Cancel. When several panels have unsaved work, one dialog lists them all.
- When the dialog cannot be shown, quitting discards the work, so quitting always finishes. Closing panels is cancelled and keeps the work.

### 4.6 Popups, pointer and text input

- **Popups.** A panel opens a popup anchored to a rectangle given from its own corner: `ECSPopup_Open(panel, &popup, &desc)`, and in Lua `panel:openPopup(desc)`, which gives the popup. A plugin may open one from any shown panel, and the anchor may lie anywhere in the panel's OS window: `ECSPanel_GetPlace(panel)`, in Lua `panel.place`, gives the panel's place in its OS window and the window's size, in the panel's surface pixels. So the standard plugin keys shows the keys after the prefix at the bottom right of the focused panel's OS window (7.5). The description holds:
  - `type`: a menu, which takes the keys, or a tooltip, which takes no keys and no pointer events: they go to what is under it, as SDL's tooltip windows let them through (`ECSPopupType_Menu`, `ECSPopupType_Tooltip`; in Lua `"menu"` and `"tooltip"`).
  - The anchor, a rectangle in the panel's surface pixels: `anchorX`, `anchorY`, `anchorWidth` and `anchorHeight`; in Lua `anchor = { x = 0, y = 0, width = 0, height = 0 }`.
  - The popup's `width` and `height`, in layout units.
  - Its functions, which get the description's `data`: `Draw` draws its pixels surface, as a panel's `Draw` does; `Event` gets its pointer, wheel and key events, with positions in its surface pixels; `Closed` says it has closed. Only `Draw` is required. In Lua: `draw(surface)`, `event(event)` and `closed()`.
  - The popup opens below the anchor, with its left edge at the anchor's, or above the anchor if there is no room below.
  - `ECSPopup_Redraw(popup)` asks for it to be drawn again, and `ECSPopup_SetSize(popup, width, height)` resizes it. In Lua: `popup:redraw()` and `popup:setSize(width, height)`.
  - A menu says which entries it shows, in order, with `ECSPopup_SetItems(popup, items, count)`, in Lua `popup:setItems(items)`: each entry's `label`, its `keys` or none, whether it is `enabled`, and whether it is `chosen`, the one that Return runs. The core copies them, and a test reads them with `test.menu` (17.5). A menu calls it again when they change, such as in its `Draw`.
  - A popup closes when code closes it (`ECSPopup_Close(&popup)`, `popup:close()`); on a press outside every popup while a menu is open, which does nothing else; on Escape, if it is a menu; and when its panel closes, fails or is hidden. Its `Closed` function runs then, and the popup is invalid afterwards.
  - While a menu is open, key presses go to the newest menu after the core prefix, which comes first (7.4) and closes the menus. A popup's events wait, like a panel's, until the current callback returns (8.2).
  - Popups are SDL popup windows (`SDL_CreatePopupWindow`), each with its own renderer. SDL keeps them inside the display and hides them with their parent. A video driver without popup windows, such as the offscreen driver of the tests, gets them drawn inside the panel's OS window instead, above everything else. Either way a popup is kept inside its OS window, so a compositor never has to move or shrink one that sticks out, and a popup window shows its pixels at their own size, never stretched: a plugin that has more to show than the window holds scrolls it, as std's dropdown does.
- **Pointer.** The pointer's shape is a system shape or an image. The pointer lock uses `SDL_SetWindowRelativeMouseMode`; pressing the core prefix also ends it.
- **Text input.** A panel says whether it accepts text and where its text cursor is, in its surface pixels: `ECSPanel_SetTextInput(panel, accept, x, y, width, height)`, and in Lua `panel:setTextInput(accept, x, y, width, height)`. The core turns text input on while the focused panel accepts text, and tells the input method where to show its window (`SDL_StartTextInput`, `SDL_SetTextInputArea`).
  - The focused panel gets the typed text as `Text` events, in UTF-8; the text is valid during the event.
  - A key press that runs the core's keys or a binding types no text (7): the core drops the text that follows it, until the key's release. A key's text comes between its press and its release, so text typed after a key that types none, such as Return in a menu, and text without a key press, such as an input method's, still arrive.
  - An input method, such as fcitx5 on Wayland, may repeat a held key itself: it sends a release and a press with the first press's own time. SDL's Wayland backend keeps that stale time, and when the key is released it sends at once every repeat it thinks it missed, so the key would be typed twice. A press with the same key and time as the release before it marks the key as repeated by the input method, and the core drops SDL's repeats of it, with their text, until the next release.

## 5. Surfaces and rendering

Every panel gets its own **surface**, which the core owns and puts on screen. The panel draws into its surface, and the core assembles all surfaces into what the user sees. So one panel cannot paint over another, and the core can move a panel to another OS window without its help. A panel type chooses pixels, memory that it fills with the processor, for image viewers, emulators or anything that computes its own pixels; or a GPU surface, an image on the graphics card, for 3D views and fast interfaces.

### 5.1 Surfaces

- A panel's corner, and a popup's, is on a whole pixel, and its pixels show one to one, so what it draws stays sharp at any place and size.
- Pointer positions given to a panel are in its surface's pixels, so they match what it draws.
- A surface's `scale` is the setting `ecs.scale`, 1 by default, for every panel and popup. `Shown` and `Resized` give a panel's size in its pixels divided by the scale, and a popup's `width` and `height` are multiplied by it. When it changes, every panel draws again, with `kept` false, and hears its new size. The core's own interface grows with it too: the sizes its settings give, such as `ecs.fontSize` and `ecs.tabRowHeight`, and its paddings and gaps are multiplied by it, and its lines stay as thin.
- A panel must not keep its surface after `Draw` returns; every call receives the current one. So the core can recreate surfaces, for example on resize, without telling the panel.
- A pixels surface keeps its pixels between draws while its size stays the same. `kept` tells `Draw` that they still hold the picture of the last `Draw`, so it may change only what changed, such as the new part of a stroke. It is false for a new surface, after a resize or a change of scale, and always for a GPU surface.
- `ECSSurface_Changed(surface, x, y, width, height)`, `surface:changed` in Lua, tells the core which rectangle `Draw` changed. The core uploads only the rectangle around every one given; a `Draw` that tells none changed the whole surface. A popup's surface is always uploaded whole.

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

    void *texture;           // GPU type: the SDL_GPUTexture to draw into, on the device that ECSGpu_GetDevice gives
    bool kept;               // pixels type: the pixels still hold the picture of the last Draw
} ECSSurface;
```

### 5.2 Pixel format

- 32 bits per pixel, `ARGB8888` in native byte order, with premultiplied alpha.
- Panels are opaque, so alpha is ignored for panels. It matters only for popups.
- A colour that a plugin gives is an `ECSColor`, `0xAARRGGBB`, not premultiplied; the core premultiplies it where it writes pixels. Lua, settings and presets write colours as texts: `"#RRGGBB"`, `"#RRGGBBAA"`, or the name of a colour of the theme, such as `"text"` for `ecs.colorText` (12.4). `ECSColor_Parse` reads such a text, and the signature type `color` (10.2) turns texts into `ECSColor` between Lua and C.
- The names of the theme's colours are the core's colour settings, `ecs.colorBackground` to `ecs.colorHover` (12.4), each without `ecs.color` and with a small first letter, such as `"tabRow"` or `"button"`. The type `ecs.Color` in `ecs.lua` lists them, so editors complete them wherever a colour goes, and it changes with the core's colour settings.

### 5.3 The renderer

- One SDL GPU device serves every OS window. An offscreen GPU renderer, `SDL_CreateGPURenderer(nullptr, nullptr)`, makes it as SDL's renderer needs it, and owns it. Each OS window has a 2D renderer on that device, made with `SDL_CreateGPURenderer(device, window)`.
- The device outlives the OS windows and the plugins: it is destroyed after every plugin's `Destroy`, so a plugin releases its GPU objects there or earlier.
- An OS window whose GPU renderer cannot be made, as when the GPU cannot present to it, uses SDL's default renderer. The device stays, and GPU panels still draw on it: that window shows a copy of each GPU panel's texture, which the GPU downloads after each draw into a texture of the window's renderer. It is slower, but GPU panels work where the GPU draws and something else presents, such as a laptop whose integrated GPU drives the screen, the offscreen windows of the tests, or Windows' software adapter. Without a GPU device, GPU panels show placeholders.
- The core draws its own interface with that 2D renderer: Clay for layout, SDL3_ttf for text.
- A GPU surface is a texture on the shared device, `B8G8R8A8_UNORM`, which is `ARGB8888` on the GPU, of the panel's size in pixels. Any OS window's renderer shows it by wrapping it (`SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER`), so a GPU panel moves between OS windows without recreating its texture. The texture is made again only when the panel's size changes, and then the panel draws before it shows.
- A pixels surface is uploaded to a texture when it changed. Plugins with pixels surfaces never notice how the core shows them; GPU panels draw into the texture themselves (5.4).

### 5.4 How plugins draw

- A GPU panel type sets `surface` to `ECSSurfaceType_Gpu`, and in Lua `surface = "gpu"`. Its `Draw` gets a surface whose `texture` is its `SDL_GPUTexture`, on the device that `ECSGpu_GetDevice` gives (9.8). What draws into it submits its commands before `Draw` returns, or in a function given to `ECSSurface_OnDrawn`, and the core's renderer shows the texture after, in the same frame.
- `ECSSurface_OnDrawn(surface, Drawn, data)` runs a function once the Draw that gave a surface returns, before the core shows what was drawn, for pixels and GPU surfaces and popups alike. A service that draws into surfaces finishes its work with it so, such as gpu sending what it drew to the GPU, and the plugin that called it makes no call of its own. Functions given for one surface run in order. It is C only; Lua's draw finishes its own work before it returns.
- The standard plugin **gpu** draws into a GPU surface from Lua or C: shapes, lines and textures, and vertex buffers with a plugin's own shaders, with no SDL in sight (20). A native plugin may instead use SDL's GPU API itself, in a command buffer of its own.
- A GPU surface has no pixels. `ECSSurface_Fill` does nothing on it, the Lua surface's pixel methods raise an error, and draw refuses it. A Lua panel type draws into its GPU surface with the gpu plugin.
- Plugins that compute their own pixels use pixels surfaces. C writes the pixels directly; Lua uses the surface's methods (11.4).
- `ECSSurface_Fill(surface, x, y, width, height, color)` fills a rectangle of a pixels surface with one colour, in pixels and clipped, such as to clear it. Lua: `surface:fill(x, y, width, height, color)`.
- The **draw** plugin draws shapes, images and text into pixels surfaces on the CPU, and the **ui** plugin builds user interfaces on it (20).

## 6. Layout

- Every OS window holds one **layout tree** of splits, groups and panels. A split divides an area horizontally or vertically, each part with a fixed size, such as a toolbar, or a share of the rest, so resizing keeps proportions. A group holds one or more panels in one place, one shown at a time. A panel type can declare a minimum size, which splits respect.
- A group with two or more panels shows a **tab row**. The user drags a tab to move its panel, right-clicks it for the panel's menu, and middle-clicks it to close the panel; a right click on the rest of the row opens the shown panel's menu, and dragging the rest of the row moves the whole group. Tabs reorder by dragging, and the mouse wheel over the row shows the next or the previous tab. A tab shows the panel's title, a mark for unsaved work and a close button.
- A group with one panel shows no tab row. Near the panel's top edge, the core shows a small **grip** with its title, which drags the panel and opens its menu (6.4).
- Every layout change is a plain function: open, split, move, group, close, pop out, swap, resize a divider, move focus, maximize, switch workspace. The mouse only works out the arguments and calls the same function that a script would call, so a menu entry or a key can always do what dragging does (6.5).
- While a panel or a group is dragged, the core highlights where it will land: the centre of a panel groups with it, near an edge splits, over a tab row inserts between tabs, near an edge of the OS window docks along that edge, and outside every OS window moves into a new one (6.7). On Wayland, the display system decides where a new OS window appears.
- A panel that a plugin opens goes to a predictable place, unless the caller names another: the group of the most recently focused panel of its type, or the focused group (6.6).
- **Maximize** makes one group fill its OS window, and **pop out** moves a panel into a new OS window with a layout of its own. Floating panels inside one OS window are not part of the design. Switching **workspaces** is instant, as nothing is rebuilt; pop-out windows belong to their workspace (6.10).
- A preset or the user can lock parts of the layout, such as a toolbar, which the user cannot move or close until they unlock it.
- A panel's menu has the core's entries, which act on the panel and its group, and those its type adds; the title bar's menus act on the whole tool. Menus offer only what can be done now (6.8).

### 6.1 Data

- Each OS window is a **root**: the OS window, its layout tree, its maximized group, if any, and its size. A workspace's first root is the main window's; it is empty when the workspace has no panels there.
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

- The layout tree fills its OS window below the title bar, less a margin of `ecs.dividerSize` layout units at each edge, as wide as the gap between groups, so no group meets the window's edge.
- If an OS window is too small for all minimums, the last children are clipped.
- Dragging a divider next to a fixed-size child changes that child's fixed size.
- Where splits meet, as a divider ends on a divider of the other direction, a press within `ecs.dividerSize` layout units of that end drags both dividers at once.

### 6.4 Grips and locked groups

- A grip appears when the pointer is within `ecs.gripZone` layout units of a panel's top edge. It shows the panel's title. While it is shown, pointer events over it go to the core.
- A locked group shows a lock mark at the end of its tab row and in its grip. Its grip opens the panel's menu, but it cannot be dragged, and pulling it is not a click.
- A maximized group shows a maximize mark the same way.
- No dropped panel joins a locked group; a panel can split beside it, which changes nothing inside it. Its panels cannot be dragged, moved with keys or closed by the user.
- `ecs.layout.lock` locks or unlocks the focused group.

### 6.5 Operations

- Besides the operations in 6.5, there are cycling tabs, reopening the last closed panel, and moving a panel to another workspace or to a new one.
- `ecs.layout.reset` builds the layout again from the preset or session the tool started from, which is read again: it asks about unsaved work, closes every panel and opens the file's panels in every workspace.
- The core remembers the last `ecs.reopenLimit` closed panels with their type and saved state. `ecs.layout.reopen` opens the last one again next to a panel that stayed in its group, wherever that panel is now, or else in the focused group, and shows its workspace.
- `ecs.layout.moveToWorkspace1` to `ecs.layout.moveToWorkspace10` move the focused panel into that workspace's focused group; the current workspace stays shown.
- `ecs.layout.moveToNewWorkspace` moves the focused panel into a new workspace, after the last one and named after the panel's title; the current workspace stays shown. A tool has at most 10 workspaces, as their functions are numbered 1 to 10, so a tenth leaves no room for a new one.
- The core never makes a panel of a type on its own initiative, other than to restore a session or reopen a closed panel: a panel's contents belong to its plugin, so a plugin that wants several panels of its type opens them itself (6.6), from its own functions, keys or menu entries.
- Plugins: `ECSLayout_Open`, `ECSLayout_Move(panel, target, ECSZone_Left)` (also from another workspace), `ECSLayout_Close`, `ECSLayout_Focus` and `ECSLayout_GetFocus`; `ECSWorkspace_Switch` and functions that count and name workspaces. Lua: `ecs.layout.move(panel, target, "left")` and so on, with panel handles.
- Workspaces are numbered from 1, in C and in Lua: `ECSWorkspace_Switch(10)` switches to workspace 10.
- Closing a panel from code still asks about unsaved work. Focusing a panel shows its workspace and its tab.
- A panel's or popup's draw that opens, moves or closes a panel misuses the function (1.4): the frame walks the layout while panels draw. It does so in an event, a timer or a function instead.
- Locks (6.4) stop the user, not code. A core function called without arguments is the user's action (7.8).
- Every change of focus tells the panel that loses it and the panel that gets it (`ECSPanelEventType_Unfocused`, `ECSPanelEventType_Focused`).

### 6.6 Placement of new panels

- The caller can name a group, a side of a panel (split), or a new OS window: `ECSLayout_Open(plugin, &panel, "text.editor", state, target, ECSZone_Right)`.
- Without a target, a new panel joins the group of the most recently focused panel of its type, or else the focused group (6.6). It opens in the current workspace and gets the focus.

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
- The pointer shows a resize arrow over a divider and while it is dragged, a four-way arrow where splits meet (6.3), and a move arrow while a panel or group is dragged.
- The drop place is highlighted while the panel is dragged.
- A panel that splits another takes half of the other's place. A panel docked along an edge of the OS window takes a quarter of it.

### 6.8 Menus

- The core draws its menus itself, inside the OS window, with Clay. They are kept inside the OS window.
- A menu is as wide as its widest entry: its label and keys, with an entry's padding at each end and between them, inside the menu's padding.
- A right click on a tab or grip, or a click on a grip, opens the panel's menu. A right click on the rest of a tab row opens the shown panel's menu. Each way opens the same menu. The panel gets the focus.
- Entries are core functions that act on the focused panel. Each shows the keys that run its function after the prefix (7.5).
- Entries are short, so menus stay narrow. Menus show only what can be done now, and say what it does now: "Lock" or "Unlock", "Maximize" or "Restore". Close and move are left out for a locked group, pop out for a locked group and for a panel alone in its OS window, restart for a panel that has not failed (14.2), reopen when nothing was closed, a new workspace when the tool has 10, and split when the panel's type is missing.
- Entries of one kind go into a submenu. An entry with a submenu shows `›`, and pointing at it opens the submenu beside it.
- The panel's menu: close, close the group's panels, restart, reload its plugin, maximize, pop out, lock, reopen the last closed panel; the submenus Tabs (each panel of the group, the shown one marked with `•`; choosing one shows it), Move (left, right, up, down) and To workspace (each other workspace, by number and name, then a new workspace); then the entries of the panel's type, under a heading with its plugin's name, which a line sets apart. A group of one panel has no Tabs and no entry that closes the group, which close does.
- The entries of a type: `ECSPanelType_AddMenuEntry(plugin, type, function)`, or `ecs.panel.addMenuEntry(type, function)` in Lua, adds a service function whose signature is `void(handle<ecs.panel>)` or `void()`. The entry shows the function's description and the key the plugin bound to the same function for the type.
- The title bar's menus (6.11) are the tool's menu: what acts on the whole tool is in them, and in no other menu. `ecs.app.menu` opens the first of them below it; with `ecs.titleBar` off, or when the window is too narrow to show them all, it shows one menu near the focused panel's top left corner, whose submenus are the title bar's menus.
- The title bar's menu Tools holds the functions that plugins add for the whole tool, by their labels, whatever order the plugins load in: `ECSApp_AddMenuEntry(plugin, function)`, or `ecs.app.addMenuEntry(function)` in Lua, adds one of the plugin's own functions, whose signature is `void(handle<ecs.panel>)` or `void()`. The entry shows the function's description and its keys after the prefix. The standard plugins add the settings window, the key list, the panel list, the plugin list, the launcher and the profiler.
- The menu of workspaces lists each workspace by number and name, marks the current one with `•`, and switches to the one chosen. The title bar's workspaces' menu opens it (6.11), and `ecs.workspace.menu` shows it near the focused panel's top left corner.
- A menu opens with its first entry chosen, and the menu of workspaces with the current workspace, so Down chooses the next one. Up and Down choose an entry. Right or Enter opens a submenu; Left or Escape closes it. Enter runs an entry. In the title bar's menus, Left and Right go to the menu beside, when they open or close no submenu. Escape in the first menu, a press outside the menus, or the core prefix (7.4) closes them. While a menu is open, pointer events go to the menus only, and so do key presses after the prefix.
- A tab's close button and a middle click on a tab close its panel; panels of a locked group have no close button.
- The wheel over a tab row shows the next tab, a step down or right, or the one before, a step up or left; it stops at the first and the last tab. When the group has the focus, the shown panel gets it. When a group shows another panel, its tab scrolls into view.

### 6.9 Clay

- The core computes the rectangles of the layout tree itself.
- Clay lays out the core's own interface on top of them: the title bar, tab rows, grips and menus. There is one Clay context per OS window (`Clay_SetCurrentContext`).
- Clay's SDL renderer draws what Clay lays out, apart from texts: it would make and lay out each text again every frame. The core keeps each OS window's texts between frames, by size and content, and destroys those that are no longer drawn once there are many; the font has one copy for each size, as a text is laid out again when its font's size changes. The width of each title bar text is kept too, until the settings change.
- Groups, and the focused panel's border, have rounded corners of 6 layout units; menus, tooltips and the dialog have 4, and the title bar's parts 3. Clay's renderer rounds borders and backgrounds, but not the panels' pictures below them, so the core places a custom element of Clay at each corner of a group and fills the room outside the corner's arc in the colour `background` itself.

### 6.10 Maximize, pop-out and workspaces

- Maximize is a mark on one group of a root. The mark is saved in the session.
- Pop-out creates an OS window with a new root that holds one group with the panel. The window has the panel's size and opens at the pointer; on Wayland, the compositor places it (6.7).
- `ecs.layout.popOut` pops out the focused panel. A panel alone in its OS window, and a panel of a locked group, stay. Code pops a panel out with the zone `ECSZone_Window`, in Lua `"window"`: `ECSLayout_Open(plugin, &panel, "text.editor", NULL, NULL, ECSZone_Window)` or `ECSLayout_Move(panel, NULL, ECSZone_Window)`.
- Inside a pop-out window, every layout operation works as in the main window: splits, tabs, maximize, dragging and docking along its edges. Panels move between OS windows by dragging or by code.
- A dragged panel lands in the OS window under the pointer. The core finds it from the windows' places on the screen. Where windows overlap, the one that last opened or got the focus counts as in front, as SDL does not tell the windows' order.
- On Wayland, an application cannot know where its windows are, and while the button is held only the window that got the press sees the pointer. So a panel dropped outside the window it was dragged from lands where the pointer enters one of the program's windows within 400 ms, and pops out otherwise; a panel alone in its OS window stays in it. The drop place is not marked in the other windows.
- Each OS window has its own renderer and Clay context. Only the main window's renderer waits for vsync (3.1), so presenting several OS windows does not wait once for each.
- A pop-out window closes when its last panel leaves it. Its close button closes its panels, after asking about unsaved work (4.5); Cancel keeps the window. A window that holds a locked group does not close, and the core tells the user to unlock the group first (6.4). Closing the main window quits, after asking about unsaved work; SDL's own quit when the last window closes is off (`SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE`), as it would answer that question.
- When the main window's last panel leaves it while pop-out windows have panels, the first pop-out window's tree moves into the main window, so the main window is never empty. When the user closes the last panel of the tool, the program quits, and the tool's last session is removed, so the next start opens the preset.

### 6.11 Window frame

- OpenECS draws the OS windows' frames itself, so they look and work the same on every system and desktop: the setting `ecs.titleBar`, on by default, makes each OS window borderless, with a title bar `ecs.titleBarHeight` layout units tall above the layout.
- The title bar shows, from the left: the tool's name; the title bar's menus, then the menu Tools; the tool's buttons; the preset's hint; and at the right, the workspaces' menu, when the tool has two or more workspaces, then the buttons minimize, maximize or restore, and close. Dragging the rest of it moves the window; the tool's name is a label, and drags it too.
- The title bar's menus come from the core's setting `ecs.titleMenus`, a list of menus such as `{ label = "File", "ecs.session.open", "ecs.session.save" }`: a click on one opens its menu below it, and while it is open, pointing at another of them opens that one instead. The core's settings file gives File (opening and saving sessions, quit) and Window (reopen, maximize, pop out, lock, move to a new workspace, reset the layout). After them, the menu Tools holds the plugins' entries for the tool (6.8), when there are any.
- The tool's buttons come from the core's setting `ecs.titleButtons`, a list that is empty in the core's settings file, so a preset adds buttons without repeating the menus. A button runs a function, as a menu entry does.
- An entry of a menu or a button is a function's name, such as `"gallery.next"`, labelled with the function's description; or a table such as `{ "gallery.next", label = "Next" }`, with its own label, which may be a symbol such as `"▶"`. An entry's `setting` names a `bool` setting: while it is true, the entry is on, as a play button is while a game plays.
- Functions that no plugin of the tool registered are left out, and so is a menu that is left empty. A menu also leaves out what cannot be done now, as other menus do (6.8); a button stays, dimmed, and does nothing, so the buttons do not move.
- A button that is on, and the menu that is open, have the colour `selected`; a menu, a button, the workspaces' menu or a window button under the pointer has the colour `hover`. An entry that is on shows `•` in a menu.
- Pointing at a button or the workspaces' menu for half a second shows a tooltip below it: what it does, and the keys that run it, after the prefix or bound for the focused panel's type (7.8).
- The workspaces' menu shows the current workspace's name and `▾`; a click opens the menu of workspaces below it (6.8).
- A hint that does not fit in its room slides while its OS window has the system's focus: it rests at its start, slides to its end, rests, and slides back. The core draws frames only while it slides.
- When the window is narrow, the hint's room shrinks first, then the tool's buttons and the menus are left out, from the last; the workspaces' menu goes when the tool's name leaves it no room. `ecs.app.menu` then shows the menus near the focused panel (6.8), and the keys still reach every function.
- The window's edges and corners resize it. The core tells SDL which part of the window is under the pointer (`SDL_SetWindowHitTest`), so the system moves and resizes the window, also on Wayland.
- While the system moves or resizes a window, it may run its own loop until the user lets go, as Windows does, and the main loop waits. The core draws a frame each time SDL asks for one from that loop, so the window's content follows its size.
- With `ecs.titleBar` off, the system draws the frame, and the title bar's menus and buttons do not show: the tool's menu (`ecs.app.menu`) and the keys reach their functions.
- `ecs.app.quit` quits as the main window's close button does, asking about unsaved work (4.5). In C: `ECSApp_Quit()`.
- The keys that move focus or panels (7.5) act within the focused panel's OS window. Focusing a panel in another OS window raises that window (7.1). When an OS window gets the system's focus, the panel that had the focus last in it gets it.
- The core's menus show in the OS window of the focused panel.
- Switching workspaces hides the pop-out windows of the old workspace and shows those of the new one.
- A session's `windows` list holds the main window's tree first, then one tree for each pop-out window (13.2). A pop-out window's tree also has `width` and `height`, its size in layout units. When the main window has no panels but pop-out windows do, its tree is an empty table. Positions are not saved, because Wayland does not let them be chosen.

## 7. Input, focus and keys

- Exactly one panel has keyboard focus. The setting `ecs.focus` chooses whether a click or the pointer's hover gives it; click is the default, and in hover mode only real pointer movement moves focus. Keys move focus to a neighbouring panel too. Pointer events go to the panel under the pointer, or during a drag to the panel where it started.
- Keys are bound in **keys tables**, which map key combinations to service functions and have one shape wherever they are written: the core's settings file, a preset and the user's settings file. They follow the layers of settings (12), so the user can change any key, and `false` removes a key that a lower layer binds.
- Plugins bind keys only for their own panel types, which work while such a panel has focus, one type or all of them. Presets and the user's settings can also bind keys for a workspace or the whole tool, to any service function: a paint preset binds Ctrl+N to `canvas.new`, so it works even while the palette has focus. The highest layer wins, then the most specific binding: a panel type, a plugin, a workspace, the whole tool. A key that no binding takes goes to the focused panel, so a panel can run its own key logic, such as modal editing, and its own sequences of keys.
- A key press that runs a binding types no text, and while the focused panel takes text, keys without Ctrl, Alt or Super go to it.
- The core takes one combination for itself, the **core prefix**, Alt+W by default: one more key after it chooses a core action, as tmux's prefix and Vim's Ctrl+W do (7.5). It always wins, and the user can also bind core actions to keys of their own.
- Bindings follow what a key means on the user's layout, not where it sits; on layouts with non-Latin letters, letter keys count as their English letters (7.3).

### 7.1 Focus

- The focus mode is the setting `ecs.focus`.
- The focused panel has a thin border in the accent, and, when its group has a tab row, its tab a line in the accent along its top.
- Keys move focus to the nearest panel in that direction that overlaps on the other axis. This does not need the tree.
- In hover mode, the pointer over a divider or a tab row does not change focus.
- On Wayland, the compositor decides whether an application may raise one of its own windows (SDL uses the xdg-activation protocol). Moving focus to another OS window is best effort there.

### 7.2 Pointer routing

- While a button is held, pointer events keep going to the panel where the press started, even outside its OS window (`SDL_HINT_MOUSE_AUTO_CAPTURE`). The press and the release of another button go to that panel too, and it keeps the pointer until the last of them is released.

### 7.3 Key combinations

- A key combination is modifiers plus one key, written as text: `Ctrl+Shift+P`. The modifiers are Ctrl, Shift, Alt and Super. The text is case-insensitive, and key names are SDL's names of physical keys, as a US keyboard labels them. The modifiers come first, each followed by `+`, and the rest is the key's name, which may hold a `+` itself: `Ctrl++` is Ctrl with the + key, and `Ctrl+Keypad +` Ctrl with the keypad's.
- Bindings match physical keys, SDL's scancodes, which are the same on every keyboard layout: a key combination is the same key press wherever it is used. A panel's key events name the physical key too; what a key types comes as text (4.6).
- Where a key is shown, such as in menus, the list of keys after the prefix, the key list and tooltips, it is shown by what it types on the user's layout.
- AltGr (`SDL_KMOD_MODE`) is never part of a binding. A key pressed with AltGr held is text.

### 7.4 Dispatch

- A key press goes to the core prefix (7.5) first, then to the bindings, chosen as in 7, then to the focused panel as a raw key event.
- While the focused panel takes text input (4.6), a key pressed without Ctrl, Alt or Super skips the bindings and goes to the panel, so typing never runs a binding.
- A plugin binding that uses the prefix's combination is never triggered and is reported. After the prefix, the next key press goes to the core.

### 7.5 The core prefix

- The prefix is the setting `ecs.prefix`. It is one key combination, never a whole modifier.
- The keys after the prefix are the `prefix` tables of the keys tables (7.8). Each layer adds to the keys of the layers below, and `false` removes a key. So presets and the user can add keys that run service functions.
- Plugins give their own functions keys after the prefix (7.8), below the core's settings file, so the core's keys win.
- While the core waits for the key after the prefix, it emits `ecs.prefix` (8.4) with the focused panel and the keys with what they do now, in the order of the core's functions below, then the others by key. Like the menus (6.8), it leaves out what cannot be done now. A plugin lists them: the standard plugin keys shows them in a popup at the bottom right of the focused panel's OS window, in sections ([OpenECS-std DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md), the key list). When the key comes, or the prefix is cancelled, the core emits `ecs.prefix` again, that it waits no more.
- The prefix and the key after it are the only key sequence the core handles.
- The core's functions after the prefix, in the order the list shows them. Their keys are in the `prefix` table of the core's settings file (12.1).

  | Function                                                        | Action                                                                                                   |
  | --------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
  | `ecs.layout.focusLeft` and so on                                | Move focus                                                                                               |
  | `ecs.layout.moveLeft` and so on                                 | Move the focused panel into the neighbouring group, or along that edge of the OS window if there is none |
  | `ecs.layout.nextTab`                                            | Show the next tab                                                                                        |
  | `ecs.layout.maximize`                                           | Maximize or restore                                                                                      |
  | `ecs.layout.popOut`                                             | Pop out                                                                                                  |
  | `ecs.layout.close`                                              | Close the panel                                                                                          |
  | `ecs.layout.closeGroup`                                         | Close the group's panels                                                                                 |
  | `ecs.layout.lock`                                               | Lock or unlock the group                                                                                 |
  | `ecs.layout.reopen`                                             | Reopen the last closed panel                                                                             |
  | `ecs.panel.restart`                                             | Restart the failed panel                                                                                 |
  | `ecs.panel.reloadPlugin`                                        | Reload the plugin of the focused panel's type (9.11)                                                     |
  | `ecs.session.save`                                              | Save the session to a file, which a save dialog asks for (13.4)                                          |
  | `ecs.session.open`                                              | Open a session or a preset, which an open dialog asks for (13.5)                                         |
  | `ecs.workspace.switch1` to `ecs.workspace.switch10`             | Switch to workspace 1 to 10                                                                              |
  | `ecs.layout.moveToWorkspace1` to `ecs.layout.moveToWorkspace10` | Move the focused panel to workspace 1 to 10                                                              |
  | `ecs.layout.moveToNewWorkspace`                                 | Move the focused panel to a new workspace (6.5)                                                          |
  | `ecs.workspace.menu`                                            | Show the menu of workspaces (6.8)                                                                        |
  | `ecs.app.menu`                                                  | Open the title bar's menus (6.8)                                                                         |

- Escape after the prefix cancels. It is not a function, so it always works.

### 7.6 Clipboard

- `ECSClipboard_SetText` and `ECSClipboard_GetText` move text; `ECSClipboard_SetData` and `ECSClipboard_GetData` move data of a MIME type, such as `image/png`. Lua: `ecs.clipboard.setText`, `getText`, `setData` and `getData`.
- The core copies what it puts on the clipboard. What a getter returns stays valid until the next clipboard call.

### 7.7 Dialogs

- `ECSDialog_Show(plugin, &desc)` shows an open file, save file or open folder dialog, with filters such as `{ "Images", "png;jpg" }`. It does not wait: the description's `Done` function gets the paths on the main thread, or `NULL` when the user cancels or the dialog fails. Lua: `ecs.dialog.show(desc, function(files) end)`.
- While a test runs, a file dialog does not show: it waits until the test answers it with `test.fileDialog` (17.5).
- `ECSDialog_ShowMessage(title, message, buttons, count, &button)` waits for the user. Lua: `ecs.dialog.showMessage(title, text, buttons)`.
- The core draws a message dialog itself, over the OS window of the event that asks, or over the main window: a shade over the window, and a box in its middle with the title, the message and the buttons. It needs no other program. SDL's system message boxes need zenity on Wayland, and a compositor may never show its window, so OpenECS would wait for an answer that cannot come.
- Return and Space press the chosen button, at first the first; Left, Right and Tab choose another, and Escape presses the last. A click is a press and a release of the left button on the same button. Closing the window, or the system's request to quit, presses the last button too.
- While a dialog waits, no plugin code runs, as with a system dialog: no timers, no events, no functions that other threads sent to the main thread (3.3), no workers' messages (3.4) and no drawing of panels. The panels show the pixels of their last frame. The OS windows' events, such as resizes, reach the main loop after the answer, and other input is the dialog's. One dialog shows at a time; a second one cannot be shown.

### 7.8 Binding keys

- Keys are bound in keys tables (7.8). A field whose value is the name of a function binds a key combination to it, and `false` removes the key from the layers below. A field named `prefix` holds the keys after the core prefix (7.5). A field named after a panel type holds the keys that work while a panel of that type has focus, and a field named after a plugin the keys that work while a panel of any of the plugin's types has focus. A plugin's name has no dot (9.2), and a panel type's has one (9.9), so the field's name tells them apart.

  ```lua
  keys = {
    ["Ctrl+N"] = "canvas.new",                     -- for the whole tool
    prefix = { G = "canvas.grid" },                -- after the core prefix
    canvas = { ["Ctrl+Z"] = "canvas.undo" },       -- while any of the canvas plugin's panels has focus
    ["canvas.view"] = { Delete = "canvas.clear" }, -- while a canvas has focus
  }
  ```

- The keys tables, from the lowest layer to the highest:

  | Layer    | Keys tables                                                                                               |
  | -------- | --------------------------------------------------------------------------------------------------------- |
  | Defaults | The core's settings file (12.1), and the keys that plugins bind for their panel types, for themselves and after the prefix |
  | Preset   | The preset's `keys`, and each workspace's `keys` (13.2)                                                   |
  | User     | The user's settings file: its `keys` for every tool, and its `keys` for the tool, whose fields win (12.3) |

- A key press runs the binding of the highest layer that binds it. Within a layer, a panel type's keys win over its plugin's, and a plugin's over the whole tool's; of the same scope, the current workspace's keys win over the preset's own. A key that the winning binding removes goes to the focused panel.
- `ECSKey_Bind(plugin, scope, key, function)`, and in Lua `ecs.key.bind(scope, key, function)`, give a default key to a function for one of the plugin's own panel types; with the plugin's own name as the scope, for all of its panel types, so a tool whose undo works in each of its panels binds it once; with the scope `prefix`, a key after the prefix to one of the plugin's own functions, such as the settings window's `,`. A plugin's key for one of its panel types wins over its key for all of them. The core's keys after the prefix win over a plugin's, and of two plugins that give the same key, the one loaded last wins. Plugins have no function for other keys.
- `ECSKey_List(list)`, in Lua `ecs.key.list()`, lists every binding of the keys tables in effect, the plugins' keys for their panel types among the defaults: its key, its function or false, its scope (the whole tool, `prefix`, a plugin or a panel type), its layer, whether it comes from the current workspace, and whether it is the binding of its key that runs in its scope. A plugin's binding that runs in its scope may still lose to one for a panel type, while a panel of that type has focus.
- `ECSKey_Set(scope, key, function)`, in Lua `ecs.key.set(scope, key, function)`, changes a key in the user's settings file, as `ECSSetting_Set` changes a setting (12.2): a function's name binds it, false removes it from the layers below, and nil takes the user's binding away. It changes the tool's own part of the file if that part binds the key in the scope already, and the part for every tool otherwise, and writes the file.
- `ECSKey_ListFunctions(list)`, in Lua `ecs.key.functions()`, lists the functions a key can run, with their descriptions and owners. So a plugin, such as the standard plugin keys, shows and changes every binding.
- A key text that is not a key combination is reported. Once the session is built, a key that runs a function its owner does not have is reported, if the owner runs (12.1).
- A function bound by name takes no arguments, or one argument: the focused panel. Its signature is `void()` or `void(handle<ecs.panel>)`.
- The core registers its own bindable actions as functions under `ecs`, for example `ecs.layout.focusLeft` and `ecs.layout.maximize`. So keys tables name them like any plugin function.
- Each of them is also a function of the `ecs` module with the same name, such as `ecs.layout.maximize()`. A key calls it with no arguments, which is the user's action: it acts on the focused panel, and locks stop it (6.4).
- Three names are functions of the `ecs` module that take an argument: `ecs.layout.close(panel)`, `ecs.session.save(path)` and `ecs.session.open(path)`. A key runs the user's action of the same name: it closes the focused panel, unless it is locked, or asks for the file with a dialog.
- C runs them by name with `ECSService_GetFunction`.

### 7.9 Drag and drop of data

- `ECSPanel_AcceptDrops(panel, types, count)`, and in Lua `panel:acceptDrops(types)`, set the types of data a panel accepts, such as `color`. A new call replaces the list. A faulted panel accepts nothing, and a restarted panel sets its list again.
- `ECSPanel_StartDrag(panel, type, value)`, and in Lua `panel:startDrag(type, value)`, start dragging data while a pointer button that was pressed on the panel is held, usually from its `PointerDown` or `PointerMove` event. The core copies the value.
- While data is dragged, the panel that started the drag gets no pointer moves. Another panel under the pointer gets them, as when no button is held, so a panel that accepts the type can show where the data will land; the focus does not move to it. The core outlines every shown panel that accepts the type, and fills the one under the pointer. Escape cancels the drag.
- On release, the panel under the pointer gets a `Drop` event if it accepts the type, even the panel that started the drag. The event holds the position and the data, which `ECSDropData_GetType` and `ECSDropData_GetValue` read during the event. A Lua panel's event has the fields `dataType` and `value`. The panel that started the drag still gets its `PointerUp`.
- Files and text dropped from other applications arrive the same way: files as `file-list`, a list of paths, and text as `text`, a string. The core learns their type only when they are dropped, so it marks no panel before.
- Data dropped on a panel that does not accept its type is dropped nowhere.

## 8. Events

A plugin can declare its own events, such as `canvas.selectionChanged`, and emit them; other plugins subscribe to them. So data that several panels show can live in a service (10), and its panels hear when it changes.

### 8.1 Types

- A panel gets the events about itself (`ECSPanelEvent`): input (key, pointer, wheel, text, drag and drop), focus, and whether it is shown, hidden or resized.
- Named events carry a value (10.3). The core emits its own (8.4), and plugins declare and emit theirs (8.3).
- Events are notifications. Handlers return nothing and cannot cancel anything.
- A panel gets `Shown` when it becomes visible and `Hidden` when another tab, workspace or maximized group hides it, and `Resized` when its size changes while it is visible. `Shown` and `Resized` carry the size in layout units. The layout checks after each pass, so a panel that was never visible gets no `Hidden`.
- A panel's event is a tagged union, `ECSPanelEvent`: its type chooses which member is set, `pointer`, `wheel`, `key`, `size`, `drop` or `text`. Every input event carries the modifiers held when it happened. A wheel amount is positive away from the user, even when the system flips the wheel.

### 8.2 Delivery

- Events are queued and delivered on the main thread after the current callback returns, never inside another event handler.
- An event that a handler emits waits for the next delivery, which follows at once.
- An event goes to its subscriptions in the order they were made. A subscription made in a handler hears the next event, not the one being delivered.
- Changes take effect immediately; only the notification waits. A closed panel leaves the layout at once, but its `Destroy` runs after the current delivery finishes, so no handler meets a destroyed panel.

### 8.3 Named events

- A plugin declares the named events it emits by their local names: `ECSEvent_Declare(plugin, "selectionChanged", description)` declares `canvas.selectionChanged`, whose name starts with the plugin's name. Only the declaring plugin emits it: `ECSEvent_Emit(plugin, name, value)`. The value may be `NULL`; the core copies it.
- `ECSEvent_Subscribe(plugin, &subscription, name, fn, data)` calls `fn(data, name, value)` for each emission; `ECSEvent_Unsubscribe` ends it. A subscriber hears the core's events, its own, and those of plugins its manifest depends on. The event must be declared first, so a plugin subscribes after its dependencies have loaded.
- Emissions are queued like every other event (8.2).
- A panel type that names an event in `redrawOn` draws its panels again at each emission, without a subscription (4.3).
- When a plugin fails, its events, its subscriptions and the subscriptions to its events are removed.
- Lua: `ecs.event.declare(name, description)`, `ecs.event.emit(name, value)` and `ecs.event.subscribe(name, callback)`, which returns a subscription with `unsubscribe()`. `callback` gets the name and the value.

### 8.4 The core's named events

Panels are named by their id (`ECSPanel_GetID`, `panel:getId()`); `ECSLayout_FindPanel(id)` and `ecs.layout.findPanel(id)` find them.

| Event                   | Value                                                                                                                                     | When                                                                      |
| ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| `ecs.panelOpened`       | `{ panel = id, type = name }`                                                                                                             | A panel enters the layout, also when a session is built                   |
| `ecs.panelClosed`       | `{ panel = id, type = name }`                                                                                                             | A panel leaves the layout                                                 |
| `ecs.focusChanged`      | `{ panel = id }`, or `{}` for none                                                                                                        | Another panel gets the focus                                              |
| `ecs.workspaceSwitched` | `{ workspace = number }`                                                                                                                  | Another workspace is shown                                                |
| `ecs.layoutChanged`     | nil                                                                                                                                       | Panels are moved, grouped, closed, maximized or locked                    |
| `ecs.settingChanged`    | `{ name = full name }`                                                                                                                    | A setting's value in effect changes                                       |
| `ecs.pluginsChanged`    | `{}`                                                                                                                                      | Plugins were enabled or disabled (9.10)                                   |
| `ecs.logged`            | `{ time = "12:30:05.123", level = "debug"\|"info"\|"warn"\|"error", plugin = name or "ecs", message = text }`                            | A line was logged, by the core, a plugin or SDL, on any thread (14.4)     |
| `ecs.prefix`            | `{ waiting = true, panel = id, keys = { { key = "M", ["function"] = name, text = what it does now }, ... } }`, then `{ waiting = false }` | The core waits for the key after the prefix, and then waits no more (7.5) |

## 9. Plugins

- A **native plugin** is compiled C code in a shared library; a **Lua plugin** is a script. They are interchangeable: a service written in Lua is called from C and the other way round, with no glue code written by hand.
- A plugin is a folder with its manifest and its code. OpenECS finds plugins in a folder the preset may name, the user's plugin folder, and the first-party plugins shipped with it (9.3).
- At start-up, OpenECS reads the manifests, works out the order from the dependencies, and loads the plugins the preset needs with those they depend on. Missing and circular dependencies are reported clearly. The user enables and disables plugins while OpenECS runs, and the session remembers it (9.10).
- What a plugin may do depends on what it touches: it receives input and binds keys only for its own panels; only a panel's own plugin sees or changes its state; any plugin may open, move, resize, focus or close any panel, and observe layout events, as the arrangement belongs to the user; and everything it registers starts with its name, such as `canvas.new`, so names cannot collide (9.9). This is not a security boundary: it keeps plugins out of each other's way by accident. Native plugins run with full access to the program.

### 9.1 On disk

```
plugins/
  canvas/
    manifest.lua   -- description and dependencies
    libcanvas.so   -- native code, for type = "native"
    init.lua       -- Lua code, for type = "lua"
```

- A plugin's code is of one kind. A native plugin's library is `lib<name>.so` on Linux (`<name>.dll` on Windows, `lib<name>.dylib` on Apple systems), and a Lua plugin's code starts in `init.lua`. Both are in the plugin's folder.

### 9.2 Manifest

```lua
return {
  name = "canvas",              -- unique, the folder's name; the prefix of everything it registers
  version = "1.2.0",            -- semantic version
  api = 4,                      -- plugin API version it was built for
  type = "native",              -- "native" or "lua"
  description = "Paint canvas", -- optional
  depends = { ui = "1.0" },     -- optional; plugin name = minimum version
}
```

- `name`, `version`, `api` and `type` are required, and `name` must be the folder's name, without a dot, as the names a plugin registers start with it and a dot (9.9). A field of the wrong type, or a field the manifest does not know, is an error too.
- Versions follow semantic versioning. A dependency's version means "this version, or a later one with the same major number": `"1.2"` accepts 1.2.0 up to, but not including, 2.0.0.
- Manifests are read as data, without running plugin code (11.2).
- A manifest describes the plugin to the core. It holds no build settings.
- Presets follow the same rules as manifests: they name the plugins they depend on.

### 9.3 Search path and load order

- Search order: the preset's directory, the user's plugin directory, then the first-party plugins shipped with OpenECS (16). The first plugin found with a given name wins, so a user can replace a first-party plugin.
- The load order is a topological order of the dependencies. A plugin that is missing, or whose version or API version does not match, is skipped, and its panels are placeholders (14.2).
- A mistake in the structure of a manifest or a preset stops the start with an error that names the file and the field, such as `workspaces[1].windows[1].panels[2].type`: a missing or wrong field, or dependencies in a cycle. Such files are wrong, not missing, so placeholders would hide the mistake.

### 9.4 Native plugin interface

- The plugin API version in the manifest is checked before the plugin's library is loaded.
- A native plugin calls the core's functions directly: `ECSPanel_SetTitle(panel, "main.c")`. The calls are resolved when the plugin's library is loaded, against the functions that the executable exports (17.2).
- A native plugin exports `ECSPlugin_Init`:

```c
/// @brief Every native plugin defines this function. The core calls it once, after it loads the plugin's library.
OPENECS_PLUGIN SHUResult ECSPlugin_Init(ECSPlugin plugin);
```

- `ECSPlugin_Init` receives the plugin's handle. Functions that act for the plugin take it first, so the core knows who is calling.
- A plugin registers things only inside `ECSPlugin_Init`.
- A plugin describes its own life, apart from its panels, as a panel type describes its panels' lives: `ECSPlugin_Register(plugin, &desc)` with an `ECSPluginDesc`, once. Its `SaveState` and `RestoreState` keep the plugin's state in sessions (13.2), and its `Destroy` runs before the program exits (2.4), in reverse load order. Each gets the description's `data`. Lua: `ecs.plugin.register({ stateVersion = 1, saveState = fn, restoreState = fn, destroy = fn })`.
- A plugin whose `ECSPlugin_Init` fails is not destroyed, so it frees what it made before it returns.
- Plugins are loaded with `SDL_LoadObject` and `SDL_LoadFunction`.

### 9.5 ABI rules

- Within one plugin API version, the public types and functions, including those from `shu.h`, do not change. A change raises `OPENECS_API_VERSION`, and the core refuses plugins made for another version (14.2).
- Adding a function, or a value at the end of an enumeration, needs no new version.
- Plain C only: no C++ exceptions and no `longjmp` across the boundary.

### 9.6 Lua plugins

- All Lua plugins share one Lua state. Each plugin's code runs in its own environment, which reads globals from the shared global table.
- A plugin gets the core's Lua names from the `ecs` module: `local ecs = require("ecs")`. There is no global `ecs`.
- The environment has its own `require`. For `"ecs"` it gives the plugin's own `ecs` table, whose functions carry the plugin, so the core knows which plugin made an `ecs` call.
- For other names, `require` first looks in the plugin's folder: `require("parts.shapes")` finds `parts/shapes.lua`, or else `parts/shapes/init.lua`. It runs the module once, in the plugin's environment, so the module can also `require("ecs")`, and gives what the module returns, or `true` if it returns nothing. Each plugin has its own modules, so two plugins can have modules of the same name.
- A name that the plugin's folder does not hold, and that is a plugin's name, gives that plugin's functions as a table, by local name: `local draw = require("draw")` gives `draw.fill` for `draw.fill` (10.5).
- Other names go to Lua's `require`.
- Lua plugins run on [LuaJIT](https://luajit.org/) 2.1, at the release that `jit.version` gives, such as `LuaJIT 2.1.1795119671`: Lua 5.1 with a compiler that makes Lua code run at nearly C's speed. It is built with `LUAJIT_ENABLE_LUA52COMPAT`, so plugins have:
  - [Lua 5.1](https://www.lua.org/manual/5.1/)'s language and standard libraries: `string`, `table`, `math`, `coroutine`, `io`, `os`, `package` and `debug`. `io` reads and writes files, and `os` renames and removes them, runs commands and gives the time and the environment.
  - [The extensions from Lua 5.2 and 5.3](https://luajit.org/extensions.html): `goto`, `load` with a mode and an environment, `table.pack`, `table.unpack`, `table.move`, `rawlen`, `__pairs`, `__ipairs`, `__len` on tables, and `xpcall` with arguments.
  - [The syntax that LuaJIT 2.1 takes from LuaJIT 3.0](https://luajit.org/extensions.html#backports): the bit operators `~ & | << >> ~>>`, the compound assignments such as `+=` and `..=`, `continue`, `?.`, `??`, `a ? b : c`, `const`, short functions (`x -> x + 1`), `!`, `&&`, `||`, `!=` and `1_000`. It has no `//`, no metamethods for the bit operators, and no compound assignment that changes its operand in place.
  - LuaJIT's own libraries: [`bit`](https://bitop.luajit.org/api.html), [`ffi`](https://luajit.org/ext_ffi.html), which calls C and makes C data, [`jit`](https://luajit.org/ext_jit.html) with its profiler [`jit.profile`](https://luajit.org/ext_profiler.html), [`string.buffer`](https://luajit.org/ext_buffer.html), [`table.new` and `table.clear`](https://luajit.org/extensions.html). `require("jit.p")`, `jit.v` and `jit.dump` find their modules in `jit/` next to the executable.
  - `utf8` from [lua-compat-5.3](https://github.com/lunarmodules/lua-compat-5.3) 0.15.1, which is Lua 5.3's [`utf8`](https://www.lua.org/manual/5.3/manual.html#6.5).
- LuaJIT has no integers: numbers are doubles, whole up to 2^53, and `math.type` and `string.pack` are missing (11.7). Lua cannot list or make folders; the standard plugin `fs` does (20.2). A plugin never calls `os.exit`, which would quit OpenECS without saving.
- What Lua plugins run on is stated, never hidden: this section names the Lua runtime and every library that comes with OpenECS and that plugins can use, with its project and version, and links to its documentation. A release that changes one says so in its description (19.5).
- `include/ecs.lua` describes the `ecs` module for editors: every function's parameters, results and documentation, in LuaLS annotations. It changes with the bindings.
- It also describes the files plugin authors write: `ecs.Manifest`, `ecs.Preset`, `ecs.Session` and `ecs.SettingsFile`. `---@type ecs.Preset` above a file's `return` gives editors completion and checks for its fields. A plugin's author adds the build's `include/` folder to `workspace.library` in the plugin's `.luarc.json`.
- A Lua plugin's `init.lua` runs once, in a protected call. An error fails the plugin.

### 9.7 Lifecycle

- The core records everything each plugin registers. If `ECSPlugin_Init` fails, the plugin's workers and background work stop as when it is disabled (3.3), its registrations are removed and it is marked failed.
- A native plugin's library is unloaded when the plugin is disabled (9.10), and at exit. Its `Destroy` stops the threads it started and waits for them, so none of its code runs after it. The functions that those threads sent with `ECS_RunOnMainThread` run after `Destroy` and before the library is unloaded. A function that the plugin's own code sends from the main thread, such as from its `Destroy`, belongs to the plugin, and is dropped once its library is unloaded.

### 9.8 The GPU device

- Every plugin is a plugin on disk; the executable contains none. The core gives native plugins its SDL objects through two parts of its interface only: `ECSGpu_GetDevice()`, the GPU device, an `SDL_GPUDevice *` or nullptr without one, and a GPU surface's `texture` (5.4).
- The core owns the device and destroys it after every plugin's `Destroy` (5.3). A plugin that draws with SDL's GPU API itself brings SDL's headers (17.2); the standard plugin gpu draws with it for others.
- Lua has neither. The standard plugin **gpu** draws into GPU surfaces from Lua and C, with an interface of its own (20).

### 9.9 Names

- Everything a plugin registers is named `<plugin name>.<local name>`: functions (`canvas.new`), panel types (`canvas.view`), handle types, events and settings (`canvas.grid`).
- A plugin declares each of them by its local name, and the core adds the plugin's name and a dot: `ecs.setting.declare({ name = "grid", ... })` declares `canvas.grid`. A local name must not start with the plugin's own name and a dot.
- A function's local name may have dots: its parts before the last dot are namespaces, such as `json` in `parse.json.read`. Lua's table of the plugin holds each namespace as a table, so `require("parse").json.read(text)` calls it; C looks it up by its full name, and the plugin's header (10.10) names it `json_read`. A plugin cannot have a function whose name is a namespace of another, such as `parse.json` beside `parse.json.read`.
- Everything else takes full names, because it may name what another plugin declared: looking up, emitting, subscribing, binding keys, presets and settings files.
- Core code never checks a plugin's name to decide what to do. First-party plugins get no special treatment.

### 9.10 Enabling and disabling plugins

- A plugin that loaded is enabled. While OpenECS runs, `ECSPlugins_Disable(name)` and `ECSPlugins_Enable(name)` disable and enable it, and `ECSPlugins_List` lists every plugin in the plugin folders (9.3), loaded or not. Lua: `ecs.plugins.disable`, `ecs.plugins.enable` and `ecs.plugins.list`.
- The change happens when the current pass of the main loop ends, so no plugin's code is running, and a plugin may disable itself. Then the core emits `ecs.pluginsChanged`.
- A plugin that an enabled plugin depends on is not disabled: the core reports it, and names a plugin that depends on it. So the plugins that use a plugin are disabled before it, and no enabled plugin holds a function or an object of a disabled one.
- The core asks about the unsaved work in the plugin's panels (4.5); Cancel keeps it enabled. Then every popup closes, and:
  1. Each of its panels saves its state, and becomes a placeholder that keeps it (14.2) and says that its plugin is disabled.
  2. Its Lua workers stop (3.4). Its background work that waits is dropped, its running work finishes, and the `done` function of each runs, with `dropped` for work that never ran; the `done` functions of its work that finished run then too, out of the core's queue (3.3). From now on `ECS_RunInBackground` refuses its work.
  3. The objects of its handle types are destroyed, and their handles become invalid (10.6).
  4. Its `SaveState` runs, and the core keeps the state. Then its `Destroy` runs (9.4).
  5. Everything it registered is removed, as for a plugin that failed (9.7). Its library is unloaded at the loop's next step, after the functions that its threads sent to the main thread ran (3.3). Its handle types and struct types stay without their methods, as other plugins' signatures and Lua's values name them.
- Enabling a plugin loads it as at start, after the plugins it depends on, which are enabled too. When it registers its handle types and struct types again, it gets them back; a struct type must have the same fields. It gets back the state it kept. Then the placeholders of its panel types are created again from their saved state. A plugin that cannot be enabled is reported, and stays disabled.
- A plugin that is not loaded, such as one the user installed (16), is enabled the same way.
- A plugin whose callback misused a function (14.2) is disabled the same way, without the question about unsaved work, as the user did not ask for it, and with the plugins that depend on it, first. Its panels keep their state in their placeholders, which show the misuse. Enabling it again brings them back.
- A session keeps the change (13.2): `disabled` lists the disabled plugins, and `enabled` the plugins the user enabled that the preset does not depend on. The next start loads the preset's plugins and `enabled`, and records the plugins of `disabled`, and the plugins that depend on them, without loading their code.

### 9.11 Reloading plugins

- `ECSPlugins_Reload(name)`, and in Lua `ecs.plugins.reload(name)`, reload a plugin when the current pass of the main loop ends. The core disables it and the plugins that depend on it as 9.10 says, without the question about unsaved work, as their panels keep their state in placeholders. Then it enables them again from their files, the plugin first. The function `ecs.panel.reloadPlugin` reloads the plugin of the focused panel's type, and the panel's menu offers it while `ecs.reloadPlugins` is on.
- The setting `ecs.reloadPlugins`, off by default, watches the files of every plugin that runs: every `ecs.reloadInterval` seconds, 1 by default, the core finds the newest change in each plugin's folder, without hidden files. A plugin reloads when its newest change differs from the one its code started with, and stayed the same since the last check, so a build that still writes its files is not loaded half done.
- A plugin that cannot be started again is reported, and its panels stay placeholders. The next change of its files, or `ECSPlugins_Reload`, starts it again, with the plugins that reloaded with it.
- While `ecs.reloadPlugins` is on, a native plugin loads from a copy of its library in its folder, named `.reload-N-` and the library's name, with N counting the copies. So its build can replace the library while it is loaded, which Windows does not allow. Unloading the plugin removes its copy, and loading a plugin removes the copies that a run left behind.

## 10. Services

A plugin offers functions to others by registering a **service**, each function with a **signature**. OpenECS checks the signature when the function is registered, and uses it to call the function from Lua or C alike.

Panels are views: data that several panels share, such as a document, a scene or the selection, lives in a plugin's service, so two panels can show one file, one canvas can appear in two workspaces, and an inspector can show what a 3D view selects. A plugin hears changes through events (8), and saves its own state into the session, which is restored before the panels, so they find their data (13.3).

### 10.1 Registration

- A plain C function can be registered without a hand-written wrapper.
- Each function also has a one-line description, so menus and key-binding editors can show it.
- Lines after the description say what the parameters and the result are, for the definition files (10.10): `@param name text` for a parameter that the signature names, and `@return text` for the result, or an output parameter's `@param`. Menus show the first line only. A line that names no parameter of the signature, or names one twice, is reported and skipped.
- In C, a function of any type is passed as `ECSFunction` and cast back to its real type by the caller.
- A function is registered by its local name (9.9): the plugin `audio` registers `play`, which others look up as `audio.play`.

```c
ECSService_RegisterFunction(plugin, "play", (ECSFunction)AudioPlay, "int(string path, float volume)",
                            "Play a sound file\n@param path The file\n@param volume From 0 to 1\n@return The sound's number");
```

```lua
local ecs = require("ecs")

ecs.service.register({
  play = { sig = "int(string path, float volume)", doc = "Play a sound file\n@param path The file\n@param volume From 0 to 1\n@return The sound's number",
           fn = function(path, volume) ... end },
})
```

### 10.2 Signature types

| Type                 | Meaning                                                                                                |
| -------------------- | ------------------------------------------------------------------------------------------------------ |
| `void`               | no value (return only)                                                                                 |
| `bool`               | true or false                                                                                          |
| `int`, `int64`       | whole numbers (11.7)                                                                                   |
| `float`, `double`    | decimal numbers                                                                                        |
| `color`              | a colour (5.2): an `ECSColor` in C, a text in Lua                                                      |
| `string`             | text ending in a zero byte                                                                             |
| `buffer`             | a `SHUSlice`, passed by value                                                                          |
| `handle<name>`       | a typed handle (10.6)                                                                                  |
| `struct<name>`       | a struct (10.11): a pointer to it in C, a table in Lua                                                 |
| `value`              | a generic value (10.3): a `const ECSValue *` in C                                                      |
| `value<Type>`        | a generic value whose Lua type is `Type`, such as `value<ui.Props>`; for definition files only (10.10) |
| `fn<signature>`      | a function to call back (10.4)                                                                         |
| `out <type>`         | an output parameter; in Lua, an extra return value                                                     |
| `keep fn<signature>` | a callback that the function keeps after it returns (10.4)                                             |

- A parameter may have a name after its type: `void(string path, float volume)`. Names only document; they never change how signatures compare. Definition files use them (10.10).
- A value may name its Lua type: `value<ui.Element>(value<ui.ButtonProps> props)`. Like a name, the type only documents, so `value<ui.Props>` and `value` compare the same, and C still gets an `ECSValue`. The type is a LuaLS type, such as a class, `string[]` or `ui.Rect?`, without `<` or `>` in it; an alias names a type that needs them.
- An object that its provider owns and frees is a handle (10.6). Plain data with fixed fields, which anyone can make, copy and keep, is a struct (10.11).
- In C, an `out` parameter is a pointer to its type. An `out value` is a value that the caller gives and the function fills.
- There is no fixed limit on the number of parameters.

### 10.3 Values

A generic value (`ECSValue` in C) is nil, a boolean, an integer, a number, a string, a buffer, a handle, or a table (a list or named fields). Saved state, settings, plugin events and generic calls use values. From Lua, a whole number is an integer, and any other number a number (11.7).

- A plugin fills a value that the core gives it, with `ECSValue_SetInteger`, `ECSValue_SetTableField` and so on. To pass a value of its own, a plugin makes it with `ECSValue_Create` and destroys it with `ECSValue_Destroy`.
- Getters take a fallback, returned when the value has another type or is missing: `ECSValue_GetInteger(ECSValue_GetTableField(state, "document"), 0)`. So saved state from an older or edited file never needs extra checks.
- Each part of a value is allocated on its own with SDL's allocator. Values are small and short-lived, so they need no arena.
- A table is a list and named fields together. From Lua, integer keys from 1 up to the first missing one are the list, and text keys are the fields. Other keys are reported and skipped.
- A buffer is bytes that may hold zeros: `ECSValue_SetBuffer` copies them, and `ECSValue_GetBuffer` reads them, or a string's bytes. From Lua, a string with a zero byte, a `string.buffer` and a cdata array become a buffer, as a buffer argument's bytes do (10.7); to Lua, a buffer is a string.
- A handle names an object of a handle type (10.6) by its type and its object: `ECSValue_SetHandle(value, "ecs.panel", panel)` and `ECSValue_GetHandle(value, "ecs.panel")`. The value does not own the object. When the object is freed while the value waits in an emitted event, is dragged or waits to be dropped, the value gives no object: `nullptr` in C, nil in Lua. To Lua it is the object's handle, and a handle from Lua stays one, so an event can name a panel by its handle. A value made from a Lua handle keeps that handle, so the object lives as long as the value.
- Files hold only data (13.4): a buffer is written as a string, and a handle is left out.
- Functions on a table's list have `List` in their names (`ECSValue_GetListCount`, `ECSValue_GetListItem`, `ECSValue_AddListItem`), and functions on its named fields have `Table` (`ECSValue_GetTableField`, `ECSValue_SetTableField`, and `ECSValue_GetTableFieldCount` with `ECSValue_GetTableFieldAt`, which go through every field).

### 10.4 Calls

- **Lua calls C:** the core makes one Lua function for each registered C function: a wrapper, Lua code written from the signature, that checks the arguments, calls the C function through LuaJIT's FFI and converts the results back. The JIT compiles the wrapper with the Lua code that calls it, so the call runs as a C call, without leaving compiled code. The wrapper also does what the core does around a call: it raises a misuse that the C function noted at the caller's line (1.4), turns `out string error` into `nil` and the message (10.8), and gives the call's time to the function's plugin (3.5). Values and structs are converted by the core's C code, which the wrapper calls through the C API.
- Lua cannot run inside a compiled call, so a C function that Lua calls through the FFI calls no Lua. These are called through the C API, as libffi calls: a function with a callback parameter; every function of a plugin whose C code may call Lua outside them, as it keeps callbacks (`keep fn`) or looked up a Lua function or such a plugin's function; and the core's own functions, which may run a plugin's Lua code, such as a panel's `create`. Such a call keeps its storage on the C stack, checks a handle by the type its signature points to, and gives a callback that is not kept the closure that the last call of its parameter left, so only its values allocate. A C function called through the FFI that calls a Lua function anyway, or a core function that runs Lua, such as opening a Lua panel, is a misuse: the Lua code does not run, and C gets zeros or a failure. A plugin's C code looks up the Lua functions it calls in its `Init`: Lua functions that Lua looked up before keep calling its functions through the FFI, and only later lookups get them through the C API.
- **C calls C:** the caller gets the raw function pointer and calls it at full speed.
- **C calls Lua:** the caller also gets a typed C function pointer: a libffi closure that converts the arguments, calls the Lua function in a protected call, and converts the result.
- **Lua calls Lua:** a plain Lua call, because all plugins share one Lua state.
- Callbacks (`fn<...>` parameters) work the same way in both directions. In C, a callback is a function pointer of its signature; in Lua, a function. `nil` in Lua is `nullptr` in C.
- A callback is valid only during the call that gives it. C must not keep it; a Lua function that keeps one gets an error when it calls it later.
- A `keep fn<signature>` callback stays valid after the call, so a service can call it later, such as when work in the background is done. The function that took it releases it when it no longer calls it: in C with `ECSService_ReleaseCallback(callback)`, which frees the closure of a Lua function; a Lua function just drops it. Callbacks that are never released are freed when the program exits, and those a failed plugin kept are freed with its registrations. A callback that a failed or disabled plugin's Lua code gave to another plugin stays valid, but calling it runs nothing and gives zeros. So does a callback that a C plugin gave to a Lua function, once that plugin fails or is disabled. A C function that keeps another C plugin's callback holds its raw pointer, which the core cannot guard: the plugin that gave it takes it back in its `Destroy`, with the function its provider has for that. A kept callback is called on the main thread only, as every callback is.
- A callback is a parameter only, never `out` or a result, and its own signature has no callbacks.
- Strings, buffers and values that a Lua function gives to C stay valid until that function returns again.
- When a Lua function called from C raises an error or gives a value of the wrong type, the error is reported (14.2) and C gets zeros.

### 10.5 Lookup

- A plugin asks for a function by name and states the signature it expects. The core compares it with the registered signature and refuses a mismatch, so a version mismatch shows up at lookup instead of crashing a call.
- A plugin may look up its own functions, and functions of plugins named in its manifest's dependencies. A provider fails only while it loads, before the plugins that depend on it, and those are skipped (14.2). So a plugin never uses a failed provider.
- C: `ECSService_GetFunction(plugin, &play, "audio.play", "int(string, float)")`. Lua: `ecs.service.get("audio.play", "int(string, float)")`, where the signature may be left out.
- Lua also gets every function of a plugin at once: `local audio = require("audio")` (9.6), with the same rule.
- C gets them at once from the plugin's definition header (10.10): `AudioFunctions audio; AudioFunctions_Get(plugin, &audio)`, then `audio.play(path, 1.0f)`.

### 10.6 Handles

- A handle stands for an object owned by its provider: a pointer plus a type name and a destructor, registered with `ECSHandle_RegisterType(plugin, "sound", Destroy)` by the plugin `audio`. In C, a `handle<audio.sound>` is the object's pointer, as `void *`. A function that takes or gives a handle is written with `void *`, the type its definition file names, as a call through a pointer of another function type is undefined.
- In Lua, a handle is cdata of LuaJIT's FFI: a struct of an FFI type that its handle type has alone, which holds the object's pointer. `ffi.metatype` gives the FFI type the handle type's metatable, with its methods, so a function's wrapper (10.4) checks a handle with `ffi.istype` and reads its object in compiled code. A handle of the wrong type is rejected with a clear error. When Lua no longer uses a handle, its garbage collector calls the destructor.
- The same object always has the same Lua handle. A provider that still uses an object after giving it to Lua counts references, and its destructor drops one.
- A handle that waits for its finalizer is already gone from Lua, so pushing its object again makes a new handle. The destructor runs once, when the object's last handle is collected.
- The core's own handle types are `ecs.panel`, Lua's panel handles (11.4), and `ecs.surface`, the surface a panel draws into. A surface handle is valid only during the `Draw` call that gives it, so a panel can pass its surface to a service that draws, such as the draw plugin's (20).
- The core's handles' methods and fields are C functions of the core that Lua calls through wrappers of the FFI, as it calls plugins' (10.4), such as a surface's `fill` and `width`. They run no Lua, so the JIT compiles them with their caller. No name finds them: they are not services, and keys do not run them. A panel's methods that take tables or callbacks, such as `openPopup`, are called through the C API.
- A Lua plugin provides a handle type too: `ecs.handle.registerType(name)`, `ecs.handle.new(name, value)` for a handle that stands for a Lua value, and `ecs.handle.value(handle, name)`, which gives the value back to the plugin that owns the type. All three take the type's local name (9.9). Users see such a handle like any other. The core keeps the value until the handle is collected.
- Handles that wait for their finalizer at exit are collected before the handle types are freed.
- A failed plugin's handles become invalid without their destructor. On exit, the objects of handles that Lua still holds are destroyed before plugins are destroyed.
- A provider may free an object before its handles are collected, such as with a plugin's `free` function: `ECSHandle_Forget(object)` makes the object's handles invalid without calling the destructor, so using one raises an error. `ECSHandle_Free(plugin, type, object)` makes them invalid and calls the type's destructor, for a plugin's `free` function.
- A handle has methods in Lua, so a plugin writes `connection:send(data)` for `net.send(connection, data)`. A provider names them: `ECSHandle_AddMethod(plugin, "connection", "send", "send")` makes its function `send` the method `send` of its type `connection`, and a Lua plugin calls `ecs.handle.addMethod`. The function must be the provider's, and its first parameter a handle of the type. A method is the function with the handle as its first argument, so it is checked and called as the function is (10.4). C calls the functions.
- The core cannot find methods by itself: a function's first parameter does not tell whether the function is about the handle, as `send` is, or only takes it, and the method's name often leaves out the type, as `size` does for `draw.imageSize`.

### 10.7 Buffers

- A buffer argument is valid only during the call. A function that needs it longer copies it.
- In Lua, a buffer argument to a C function is a string, a `string.buffer`, or a cdata array or struct, but not a handle: a handle's fields are read-only, and its bytes are no data. A string is copied, as Lua's strings never change. A `string.buffer` and a cdata give their own bytes, without a copy, and the function may write into them. Memory at a pointer goes as a `string.buffer` that `buffer:set(pointer, length)` makes. A Lua function gets the argument as its caller gave it.
- A returned buffer's lifetime is documented by its function. Lua callers always receive a copy.

### 10.8 Errors in services

- Services follow 1.4. A function that can fail has `out string error` as its last parameter; a signature that has it anywhere else is a misuse. It fills `error` with `nullptr` when it works, and gives nothing else when it fails: `nil`, `false`, zero or an empty buffer.
- Lua gets `nil` for each result of a call that failed, and the message last, whatever the results' types: `local image, error = image.load(path)`. A Lua function that fails returns `nil` for its results and the message last, and C gets zeros for them.
- A C service fails with `ECS_Fail(plugin, retError, format, ...)`, and a Lua one with `return nil, ecs.fail(message)`. Both log the message as a warning, in the plugin's name, while the setting `ecs.logServiceErrors` is on, as it is by default. So a plugin never logs its own failures, and a tool that expects them turns the setting off. The message stays valid until the next `ECS_Fail` on the same thread, so a caller that keeps it copies it.
- A service checks its arguments with `ECS_Check(plugin, condition, format, ...)` in C and `ecs.check(condition, message)` in Lua (1.4). The core checks each argument's type by the signature before the call, so a service checks only what its values mean, such as an image's bytes against its size.
- `ECS_Check` gives `true` when the condition holds. When it does not, it reports the misuse, makes the callback that runs fail (14.2) and gives `false`, and the function returns at once, giving nothing. When Lua made the call, the core raises the error after the C function returns, so the Lua code stops at its line. `ecs.check` raises the error itself.

### 10.9 libffi

- Each function's libffi call description is prepared once, at registration. Its wrapper for the FFI (10.4) is made when a plugin first asks for the function.
- The core cannot check that a C function really matches its declared signature. A wrong declaration is a bug in the plugin and may crash the program.

### 10.10 Definition files

- `OpenECS --definitions FOLDER` writes the plugin interface, `OpenECS.h`, `shu.h` and `ecs.lua`, whose bytes the build puts into the executable so they always match it. Then it loads every plugin it finds, without a display, writes two definition files for each, and exits. Nothing is opened or saved. With `--preset`, it loads that preset's plugins only.
- The build writes them into `build/<type>/include/`, after the standard plugins and before the examples, so an example's C plugin includes their headers, such as `ui.h`, with that folder alone, as any plugin's author does. It also writes a `.luarc.json` in OpenECS, `std/` and `examples/` that names that folder, so editors complete `require("ui")` there with no setup. A release's archive holds the same `include/`, so plugin authors get everything with it alone, and `--definitions` writes it again from any OpenECS, with the definitions of the plugins installed there.
- `NAME.lua` is a LuaLS meta file. Its functions have the descriptions, parameter names and types of their signatures, with what each parameter and result is (10.1), so an editor that has `FOLDER` in `workspace.library` completes and checks `require("NAME")`. Each namespace (9.9) and each handle type of the plugin is a class there, with its functions or methods, so an editor completes `parse.json.read` and `connection:send` too.
- `NAME.h` is a C header. Its structure is named after the plugin in PascalCase, such as `SketchCFunctions` for `sketch_c`, and has one typed function pointer for each function, with its description and what each parameter and the result are. `SketchCFunctions_Get(plugin, &functions)` looks them all up with their signatures.
- A parameter without a name is called `arg1`, `arg2` and so on. In C, an output parameter is `ret` and its name with a capital first letter, such as `i32 *retWidth` for `out int width`; in Lua it is a result and keeps its name.
- A plugin's folder may hold `types.lua`, a LuaLS meta file of annotations only, which the plugin never runs. `NAME.lua` ends with it, so the classes that `value<Type>` names are there, such as ui's `ui.ButtonProps` with its fields.
- A plugin's folder may also hold `header.h`, which `NAME.h` ends with. It gives C plugins what the functions alone do not, such as types, inline functions and macros that call the functions, like ui's `UI_COLUMN`. It may use the structure and the functions above it.
- A struct type (10.11) is a class in `NAME.lua`, with its fields, methods and operators, and a structure in `NAME.h`, named after its full name in PascalCase, such as `GeometryVec2` for `geometry.vec2`. Its fields are in their order, so it has the core's layout. A `struct<geometry.vec2>` parameter is `const GeometryVec2 *`, and an `out struct<geometry.vec2>` is `GeometryVec2 *`.
- Types: `int` and `int64` are `integer` in Lua and `i32` and `i64` in C, `float` and `double` are `number` and `f32` and `f64`, `color` is `ecs.Color` and `ECSColor`, `buffer` is `SHUSlice` and, in Lua, `string|string.buffer|ffi.cdata*` as a parameter and `string` as a result, and `value` is `any` and `const ECSValue *`, or the type that `value<Type>` names in Lua. The core's handles are `ecs.Panel`, `ecs.Popup` and `ecs.Surface` in Lua and `ECSPanel`, `ECSPopup` and `ECSSurface *` in C; other handles are their type in Lua, such as `net.connection`, and `void *` in C.

### 10.11 Structs

- A struct is plain data with fixed fields, such as a vector or a rectangle. A function takes and gives it by value: a copy goes in and a copy comes out, so nobody owns it and it has no destructor.
- A provider registers a struct type with its fields, before the functions whose signatures name it: `ECSStruct_Register(plugin, "vec2", "float x, float y")` by the plugin `geometry`, which signatures name `struct<geometry.vec2>`. A Lua plugin calls `ecs.struct.register(name, fields)`. Struct types and handle types share their names, so a name is one or the other.
- A field is `bool`, `int`, `int64`, `float`, `double`, `color` or another struct, `struct<name>`. A field may be a list of a fixed length: `float m[16]`. A struct of one list without a name, `"float[16]"`, is that list itself in Lua, such as a matrix.
- In C, a struct is a structure with the fields in their order, laid out as C lays them out, with each field at its natural alignment. A `struct<name>` parameter is a pointer to it, valid only during the call, and an `out struct<name>` a pointer to the struct that the function fills. A struct is never a result, as C returns structures in registers that differ on each system; an `out` parameter gives it.
- In Lua, a struct is cdata of an FFI type that its struct type has alone, laid out as its C structure, so Lua reads and writes its fields as C does: `v.x = 3`. `ffi.metatype` gives it its type's methods and operators. A `bool` field is a boolean, an `int64` field one of LuaJIT's 64-bit integers, and a `color` field the colour's 32-bit number. A list struct is its list, from position 1: `m[13]` and `#m`. `pairs` gives a struct's fields in their order, or a list's positions and values.
- A function takes a struct as it is, by pointer, and gives a new one. It also takes any table with the fields, such as `geometry.length({ x = 3, y = 4 })`, and reads only those fields. A missing field, or one of another type, raises an error that names it.
- `ecs.struct.new(name, fields)` makes a struct from a table of its fields, or a copy of a struct of its type. A struct that Lua code gives as a table, such as a Lua provider's result or a saved state that was read back, stays a table until it goes through a function or `ecs.struct.new`.
- A struct has methods as a handle has (10.6): `ECSStruct_AddMethod(plugin, "vec2", "length", "length")`, or `ecs.struct.addMethod` in Lua, makes the function `length`, whose first parameter is a struct of the type, the method `length`. A method named after one of Lua's operators is that operator too: `add` is `a + b`, `sub` is `a - b`, `mul`, `div`, `mod`, `pow`, `unm`, `concat`, `eq`, `lt`, `le`, `len` and `tostring` likewise. C calls the functions. Without `eq`, a struct is equal only to itself, and `==` with a value that is not a struct of its type is false.
- Values (10.3), saved state and files hold a struct as the table of its fields.

## 11. Lua

### 11.1 Protected calls

Every call from the core into Lua is a protected call. A caught error becomes an error report (14), never a crash.

### 11.2 Data files

- Manifests, presets, sessions and settings files are read as data. They run without `ecs`, so they cannot call the core or plugins while they are read.
- They are loaded as text only (`load(text, name, "t", env)`). The environment holds Lua's basic functions and the `string`, `table`, `math` and `utf8` libraries; there is no `io`, `os` or `require`. So all of these files can use loops and conditions.

### 11.3 The `ecs` module

| Table                         | Contents                                         |
| ----------------------------- | ------------------------------------------------ |
| `ecs.panel`                   | panel types and panel functions                  |
| `ecs.layout`                  | layout operations                                |
| `ecs.workspace`               | workspaces                                       |
| `ecs.key`                     | key bindings                                     |
| `ecs.event`                   | declare, emit and subscribe                      |
| `ecs.handle`                  | handle types of Lua plugins (10.6)               |
| `ecs.struct`                  | struct types (10.11)                             |
| `ecs.timer`                   | timers                                           |
| `ecs.worker`                  | Lua scripts on threads of their own (3.4)        |
| `ecs.service`                 | register and look up functions                   |
| `ecs.setting`                 | declare, get, set, list and explain              |
| `ecs.session`                 | saving, opening and listing sessions and presets |
| `ecs.plugin`                  | information about plugins                        |
| `ecs.clipboard`, `ecs.dialog` | clipboard and dialogs                            |
| `ecs.log`                     | `debug`, `info`, `warn` and `error`              |
| `ecs.fail`, `ecs.check`       | a service's failure and a check of misuse (10.8) |

- `ecs.plugin` holds the plugin's `name`, `version` and `folder`, and `register(desc)`, which describes the plugin's own life (9.4). The `folder` is the full path of the folder the plugin was loaded from, ending with a separator, so the plugin finds its own files, such as images. In C: `ECSPlugin_GetFolder(plugin)`.

### 11.4 Panels in Lua

- A Lua panel type is a table (4.1). The core registers C callbacks that call its Lua functions in protected calls.
- Panels are handles. What a panel is, is a field: `panel.id`, `panel.type`, `panel.title`, `panel.unsaved` and `panel.place` (4.6), of which `title` and `unsaved` can be set: `panel.title = "main.lua"`. What a panel does, is a method: `panel:redraw()`, `panel:setTextInput(...)`, `panel:acceptDrops(types)`, `panel:startDrag(type, value)` and `panel:openPopup(desc)`. Each has one form only. A handle of a destroyed panel raises an error when it is used.
- `draw(state, surface, seconds)` gets a surface handle (10.6) with `width`, `height` and `scale`, and the methods `fill(x, y, width, height, color)` (5.4), `setPixel(x, y, color)`, `getPixel(x, y)` and `setRow(y, bytes, x)`. Colours are texts (5.2), and `getPixel` gives `"#RRGGBBAA"`; `setRow` takes the row's pixels as a buffer (10.7), ARGB in native byte order. Pixels outside the surface are clipped. A surface is valid only during the call.
- `event(state, event)` gets a table: `type` (such as `"pointerDown"`), the booleans `shift`, `ctrl`, `alt` and `super`, and the fields of its type: `x` and `y` for pointer and wheel events, `button` for pointer presses, `scrollX` and `scrollY` for the wheel, `key` (SDL's key name) for keys, `width` and `height` for `shown` and `resized`, `x`, `y`, `dataType` and `value` for `drop` (7.9), and `text` for `text` (4.6).

### 11.5 Parity

- The documentation of each public function in `OpenECS.h` ends with a line that names its Lua counterparts in `include/ecs.lua`, such as `/// @lua panel.title`, or says `none:` and why.
- The functions of values (10.3) have no `@lua` line, because Lua passes its own values.
- A function of `ecs.lua` that C does not have says `Lua only:` and why in its documentation, or `Keys can run it.` when it is a core function that C runs by name (7.8).
- A change that renames or changes anything plugins use changes both files, and every reference to it in code, presets, examples, settings files and documents, in the same commit.

### 11.6 Keeping Lua values

Lua functions and values that C code keeps are stored in Lua's registry and referenced by a number. They are released when their owner goes away.

### 11.7 Numbers

- LuaJIT's numbers are doubles. A number is an integer when it is whole: `n % 1 == 0`. Doubles hold every whole number up to 2^53 exactly.
- So a setting of type `integer`, an `int` or `int64` parameter and a value's integer (10.3) take a whole number, and refuse one with a fraction. An `int` must fit in 32 bits.
- An `int64` parameter, or struct field, also takes an `int64_t` or `uint64_t` cdata of the FFI, such as `123LL`, for a whole number past 2^53. Results and values give numbers.
- The bit operators work on 32 bits and give signed results: `0xFF000000 | 0` is `-16777216`. Where a result must be positive, `% 2^32` makes it so. An FFI array of `uint32_t` keeps such a number as its 32 bits, so pixels and colours are built there (1.1).
- Files written by the core write a whole number without a fraction, and others in the shortest form that reads back exactly (13.4).

## 12. Settings

Settings come in four layers, each above the ones before it: the defaults of the core's settings file and of the plugins, the preset, the user's settings file, and what a plugin sets for the run only, which no file keeps. Settings have types, so a wrong value in a file is reported and the default used. Settings of plugins that are not loaded are kept. The user's file holds settings for every tool and, apart, for one tool, and can name extra plugins to load in every tool, such as the user's own Lua scripts. The settings window changes the user's file, which the user can also edit by hand; the window rewrites it, so comments are not kept.

### 12.1 Declaring

- Plugins declare settings with a name, a type, a default and a description: `ECSSetting_Declare(plugin, &desc)`. The core declares its own with a name, a type and a description; their defaults are in the core's settings file. Owners are told when their settings change: the description's `Changed` function runs after the queued events (3.1, step 4), and only when the value in effect really changed. Then the core emits `ecs.settingChanged` with the setting's full name, so any plugin can follow a setting it does not own, such as `ecs.font`.
- Types: `bool`, `integer`, `number`, `string`, `choice` (one of a list), `key` (a key combination, 7.3), `color` (`"#RRGGBB"` or `"#RRGGBBAA"`, 5.2), `list`, `table` and `path` (a text that names a file, which the settings window lets the user choose with a file dialog). A value must have its setting's type: a `key` must read as a key combination, and a `color` as a colour.
- The core's settings hold the core's choices of look and behaviour: the prefix, focus and vsync, whether services' failures are logged, the window's size, font and colours, the title bar's menus and buttons (6.11), the sizes of tab rows, dividers and grips, the distances that start drags and drops, and the reopen limit.
- The core's settings file is `resources/settings.lua` next to the executable, in the format of the user's file (12.3). It is part of the defaults layer (12), the only place that gives the core's settings their defaults, and holds the core's default keys (7.8). OpenECS does not start if the file cannot be read, or if it gives a core setting no value of its type. Its `ecs.prefix` must be a key combination, because a prefix that cannot be read falls back to it.
- `ECSSetting_Get(name)` returns the value in effect as a value (10.3). It comes from the highest layer that sets the setting with a value of its type; otherwise it is the default. A value of another type is reported with its file and skipped.
- A name's owner is the text before its first dot: `ecs` for the core, or a plugin. Once the plugins are loaded, a setting in a file that is not declared is reported with its file if its owner runs: the core, or a plugin that loaded and did not fail. So a misspelt name is noticed. The setting is kept (0.6).

### 12.2 Interface

- `get(name)`, `set(name, value)` (writes the user's settings file), `list()` (every declared setting) and `explain(name)`: the setting's description with its default, the value in effect, the layer it came from (`default`, `theme`, `preset`, `user` or `run`), and what each layer says. An explanation is the description that declared the setting plus where its value comes from, so `ecs.SettingExplanation` extends `ecs.SettingDesc` in `ecs.lua`. In C: `ECSSetting_Get`, `ECSSetting_Set`, `ECSSetting_List` and `ECSSetting_Explain`; lists and explanations are values.
- `set` changes the tool's own part of the user's settings file if that part already has the setting; otherwise it changes the part for every tool. `set` with nil takes the setting out of both parts, so the value of the layers below the user's applies: the theme's, the preset's or the default. It rewrites the whole file (13.4), so comments in it are not kept. Without a configuration folder, as in a test, the change lasts until the program exits. So it does when the file is there but cannot be read: the file is reported and never written over, so the user corrects it and loses nothing.
- `setForRun(name, value)`, in C `ECSSetting_SetForRun`, sets a setting in the run's layer, above the user's, which no file keeps: a plugin shows a state with it, such as an editor that tints `ecs.colorFrame` while a game plays, and the user's own value comes back when it sets nil, or when the program exits. An explanation names its layer `run`.
- The settings window uses `list`, `explain` and `set` (12.5).

### 12.3 User files

The user's settings file is `settings.lua` in the configuration folder (16). The part for one tool is keyed by the tool's app id and wins over the general part of the same file. A field whose name has a dot is a setting.

```lua
-- ~/.config/OpenECS/settings.lua
return {
  ["ecs.focus"] = "hover",
  keys = { ["Ctrl+Alt+T"] = "terminal.open", prefix = { Q = false } }, -- keys for every tool (7.8)
  plugins = { "my-scripts" },                                         -- extra plugins for every tool
  tools = {
    ["org.example.Paint"] = { ["canvas.grid"] = false, keys = { ["canvas.view"] = { G = "canvas.grid" } } },
  },
}
```

- Like the user's files, the core's settings file can name `plugins` that every tool loads. The shipped file names `settings`, the settings window (12.5), `keys`, the key list, which also lists the keys after the prefix (7.5), `panels`, the list of panel types (12.6), `plugins`, the list of plugins (12.7), `launcher`, the list of presets and sessions (13.8), so every tool can go to another one, and `profiler`. Each gives its own function a key after the prefix and an entry in the tool's menu (6.8, 7.8); the file names no plugin's function.

### 12.4 Themes

- A theme is a named set of settings, usually the core's colours. The core's settings file holds these, in its `themes` table; the user's settings file can add to them, or add themes:

  | Theme               | Is                                                                                       |
  | ------------------- | ---------------------------------------------------------------------------------------- |
  | `dark`, `light`     | Plain greys with a blue accent: what `system` chooses from.                             |
  | `deepDark`          | Dark, with black behind the panels, for a dark room or a screen whose black is off.      |
  | `highContrastDark`  | White text on black, light frames and a yellow accent, each text at least 7 to 1.        |
  | `highContrastLight` | Black text on white, dark frames and a dark blue accent, each text at least 7 to 1.      |
  | `midnight`          | Dark, with navy greys and a light blue accent.                                           |
  | `ember`             | Dark, with warm brown greys and an orange accent.                                        |
  | `forest`            | Dark, with green greys and a green accent.                                               |
  | `paper`             | Light, with warm, paper-like greys and a brown accent.                                   |

- The setting `ecs.theme` chooses one: `system`, the default, follows the system's light or dark mode (`SDL_GetSystemTheme`) and changes with it, between `dark` and `light`; any theme's name chooses that theme. Its choices are `system`, then every theme of the settings files, by name, so a theme that the user's file adds is chosen as the core's are. A system that does not say, or a name that no theme has, gets the theme that `system` would.
- The chosen theme is a layer of its own, between the defaults and the preset: a preset or the user can still set a colour, and it wins over every theme.
- Plugins name a colour of the theme by its name, such as `"accent"` (5.2), so they follow the theme too.
- The theme's colours have these jobs, which the core and the standard plugins keep, so a tool's parts look alike:

  | Colour                       | Is                                                                                                   |
  | ---------------------------- | ---------------------------------------------------------------------------------------------------- |
  | `background`                 | The title bar, and the room between panels.                                                          |
  | `frame`                      | The line around each group, menu and field, and between the parts of a view.                         |
  | `tabRow`, `tab`, `tabShown`  | Tab rows and the headers inside panels; the tabs that are not shown; the shown tab.                  |
  | `panel`                      | What a panel's view is drawn on.                                                                     |
  | `field`                      | What the user types into or sets: fields, check boxes, and the tracks of sliders and scroll bars.    |
  | `button`, `buttonHover`      | Buttons and the other controls that are clicked, and one under the pointer.                          |
  | `hover`                      | A row or a menu entry under the pointer.                                                             |
  | `selected`                   | The chosen item, menu entry or text.                                                                 |
  | `accent`                     | The focus, links, keys, headings and what is on, such as a pressed button.                           |
  | `text`, `textDim`            | Text, and text of less weight, such as notes and keys.                                               |
  | `overlay`, `drop`, `placeholder` | Menus, grips and tooltips; the place where a dragged panel lands; a panel whose type is missing or that failed. |
  | `error`, `warning`           | Errors, such as a line of the log or a value that is not valid, and warnings, as text on any of the colours above. |

- `dark` and `light` keep one order of greys, so areas, headers and controls stand apart without lines: `background` is the darkest, then `tabRow`, then `panel` with `tabShown`, which is the same, so the shown tab opens into its panel. `field` is darker than `panel` in the dark theme and lighter in the light one, so what the user types stands out, and `button` is lighter than `panel` in both. The greys have no tint. `selected` is a muted blue, under text, and `accent` a brighter blue. `text` on `panel` has a contrast of at least 7 to 1, and `textDim`, `accent`, `error` and `warning` at least 4.5 to 1.
- The other shipped themes keep the same jobs and the same contrasts, on `panel`, `field` and `tabShown`, and tint their greys; the two of high contrast keep 7 to 1 for every text.
- The interface is dense, as an editor's is: the font is 13 layout units, a tab row 24 and the title bar 28, and a control is a line of text with 3 units above and below it, so a list shows many rows.

### 12.5 The settings window

The settings window is the first-party Lua plugin `settings`, in OpenECS-std (17.7). Its design is in [OpenECS-std DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md), in the section The settings window.

### 12.6 The panel list

The panel list is the first-party Lua plugin `panels`, in OpenECS-std (17.7). It lists the panel types of the tool by plugin, from `ECSPanelType_List` (`ecs.panel.listTypes` in Lua), and opens the one the user chooses. It also gives a blank panel, which holds a place in the layout. Its design is in [OpenECS-std DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md), in the section The panel list.

### 12.7 The plugin list

The plugin list is the first-party Lua plugin `plugins`, in OpenECS-std (17.7). It lists every plugin in the plugin folders with `ECSPlugins_List` (`ecs.plugins.list` in Lua), and enables or disables the one the user chooses (9.10). Its design is in [OpenECS-std DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md), in the section The plugin list.

## 13. Presets and sessions

- A **preset** is a hand-written Lua file that describes a tool: its identity, a name, an icon and an application id, so each tool is its own application on the desktop; the plugins it needs; settings and keys; and its workspaces and panels. A preset assembles the tool, so it may do what plugins may not, such as binding keys for the whole tool. It may use loops and conditions, but cannot call the core or plugins while it is read (11.2).
- A **session** has the format of a preset, and OpenECS writes it. A tool starts from its preset; with the setting that keeps sessions, off by default, OpenECS saves the session when the user quits and restores it the next time. Nothing is saved while OpenECS runs. The user can save a session to a file and open it later. A session is a snapshot, with no link back to its preset.
- Applying a session destroys nothing it cannot understand: a missing panel type gets a placeholder that keeps the panel's saved state, and one bad panel does not stop the rest. Each panel type and plugin owns its state's format, with a version, and is told which version it reads.

### 13.1 Format

Presets and sessions share one format: a Lua file that returns a table.

### 13.2 Fields

```lua
return {
  format = 3,                                -- version of this file format
  name = "paint",
  version = "1.0.0",
  app = { id = "org.example.Paint", name = "Paint", icon = "paint.png",
          categories = { "Graphics" }, mimeTypes = { "image/png" } },  -- the tool's desktop entry (13.9)
  depends = { canvas = "1.2", palette = "1.0" },
  pluginsDir = "plugins",                    -- optional extra plugin directory
  open = "canvas.open",                      -- receives files from the command line
  settings = { ["canvas.grid"] = true },
  keys = { ["Ctrl+N"] = "canvas.new" },      -- keys for the whole tool (7.8)
  workspaces = {
    { name = "drawing",
      windows = {                            -- one layout tree per OS window: the main window first, then pop-out windows
        { split = "horizontal",
          { size = 240, panels = { { type = "palette.view" } } },
          { share = 1,  panels = { { type = "canvas.view", state = { document = 1 } } } },
        },
        { width = 300, height = 400, panels = { { type = "layers.view" } } },
      },
    },
  },
}
```

- `format`, `name`, `version`, `app` with its `id` and `name`, `depends` and `workspaces` are required. Every field is checked when the file is read: a missing or wrong field, or one the format does not know, stops the start with an error that names its path (9.3).
- A layout node is a split (`split` plus its children) or a group (`panels`, and optionally `shown` and `locked`). A split's child has a fixed `size` in layout units or a `share`.
- A workspace can have its own `keys`, a keys table (7.8).
- A panel's saved state is its `state` field, and the state's version is `stateVersion`.
- A panel's `id` that another panel has, or that is out of range, is reported, and the panel gets a new one.
- Sessions also store each panel's `id`, each workspace's `focus` (a panel id), each group's `shown` panel and `maximized` mark, the `currentWorkspace`, and `pluginState`: for each plugin's name, its `state` and `stateVersion`.
- `enabled` and `disabled` are lists of plugins' names, which enabling and disabling plugins write (9.10).
- A plugin saves state of its own, apart from its panels', with the `SaveState` and `RestoreState` functions of its description (9.4). The state of a plugin that is not loaded stays in the session.
- The app id matches the name of the tool's `.desktop` file (13.9).
- `app.icon` is a path relative to the preset's folder. `app.categories` and `app.mimeTypes` are lists of texts for the tool's desktop entry (13.9).
- OpenECS reads formats 1 to 3, and writes format 3. Format 2 adds `app.categories` and `app.mimeTypes`, and format 3 adds `enabled` and `disabled`.
- A preset with `listed = false` is left out of the list of presets (13.7), as the launcher's own preset is.
- A preset's `hint` tells a new user what to try first, such as the keys to press. The title bar that OpenECS draws (6.11) shows it after the tool's menus and buttons, in dim text; one that does not fit slides. With the system's title bar, it does not show. A session keeps it, as it keeps the preset's other fields.

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
- `pluginsDir` is written as an absolute path, resolved from the preset's folder and the current folder, so a session opened from another folder, such as the launcher's, finds the preset's plugins.
- A file is written to a temporary file, then renamed over the old one, so it is never left half-written.
- `ECSSession_Save(path)`, and in Lua `ecs.session.save(path)`, write the session to a file at any time. Keys run `ecs.session.save`, which asks for the file with a save dialog that starts in the folder of saved sessions (16) and suggests a file named after the preset. A chosen name without `.lua` gets it.
- The core's setting `ecs.keepSession`, off by default, keeps the tool's session between runs: quitting saves it as the tool's last session, and the next start of the same tool opens it instead of the preset. The core reads the setting before it chooses what to open, and again when the tool quits. A last session that cannot be read, such as one of a newer format, is reported and renamed with `.unreadable` after its name, and the tool starts from its preset.

### 13.5 Opening a session or a preset

- `ECSSession_Open(path)`, and in Lua `ecs.session.open(path)`, open a session in place of the current one. They refuse a preset, which `ECSSession_OpenPreset` opens. Keys run `ecs.session.open`, which asks for the file with an open dialog that starts in the folder of saved sessions (16).
- `ECSSession_OpenPreset(nameOrPath)`, and in Lua `ecs.session.openPreset(nameOrPath)`, open a preset's tool in place of the current one. The name is looked up as `--preset` looks it up (13.6). The tool starts as at a start from the desktop: from its last session if `ecs.keepSession` is on and it has one (13.4).
- A file without a list of workspaces is reported, and nothing changes.
- The core asks about unsaved work (4.5). When the dialog cannot be shown, the work is kept and nothing is opened.
- When the current pass of the main loop ends, OpenECS shuts down from step 2 of 2.4, so the tool's last session is saved if `ecs.keepSession` is on. Then the program replaces itself with `OpenECS --session FILE`, or `OpenECS --preset FILE` for a preset. So the identity, plugins and settings of the new tool apply as at a start.
- A test cannot open a session or a preset (17.5).

### 13.6 Command line

```
OpenECS [-p|--preset NAME|FILE] [-s|--session FILE] [-f|--fresh] [-t|--test FILE] [-d|--definitions FOLDER] [-i|--install] [-u|--uninstall] [-v|--version] [-h|--help] [FILE...]
```

- `--version` prints the version of OpenECS (19.1) and of the plugin API, and exits.
- `--help` prints the options, and exits.
- Each option has a short form of one letter. An argument that starts with `-` and is not an option, or an option without its value, is reported with the help, and OpenECS exits with status 2.
- `--fresh` starts from the preset even when `ecs.keepSession` is on (13.4).
- `--test` runs a test (17.5).
- `--definitions` writes the definition files of the functions of every plugin it finds, and exits (10.10).
- `--install` adds the preset's tool to the system's application launcher, and `--uninstall` removes it; both exit without starting the tool (13.9).
- Without `--preset` and `--session`, OpenECS starts with the preset `launcher` (13.8).
- A preset's name is looked up in the user's presets, then in the first-party presets (16). A name that has a `/` or ends with `.lua` is a path.
- A session's identity wins over the preset's, so the session is saved again as the last session of its own tool, if `ecs.keepSession` is on.
- Files are passed to the function that the preset or session names in `open`. Its signature is `void(string)`. It is called once for each file, in order, once the session is built (2.3). Without an `open` function, the files are reported and not opened.
- A tool's `.desktop` file runs, for example, `OpenECS --preset paint %F` (13.9).

### 13.7 Lists of presets and sessions

- `ECSSession_ListPresets(&list)`, and in Lua `ecs.session.listPresets()`, list the presets that `--preset` finds by name (13.6). A name in both folders is listed once, from the user's presets.
- `ECSSession_ListSessions(&list)`, and in Lua `ecs.session.listSessions()`, list the `.lua` files in the folder of saved sessions (16).
- Both lists are sorted by name. Each entry is a table:

  | Field              | Holds                                                                                                  |
  | ------------------ | ------------------------------------------------------------------------------------------------------ |
  | `name`             | The file's name without `.lua`. For a preset, it is the name that `--preset` takes.                    |
  | `path`             | The file's path.                                                                                       |
  | `appID`, `appName` | The tool's identity (13.2).                                                                            |
  | `lastUsed`         | Presets only: when the tool's last session was written, in seconds since 1970. Missing if it has none. |
  | `saved`            | Sessions only: when the file was written, in seconds since 1970.                                       |

- A file that cannot be read is reported and left out, and so is a preset with `listed = false`.
- A test lists only the first-party presets, and the sessions in the folder that its `sessions` field names (17.5).

### 13.8 The launcher

When the command line names no preset and no session, OpenECS starts with the first-party preset `launcher`. Every tool loads the plugin `launcher` too (12.3), and `launcher.open`, the core prefix and `/` by default, opens its list in any tool. The preset and its plugin `launcher` are in OpenECS-std (17.7); their design is in [OpenECS-std DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md), in the section The launcher.

### 13.9 Desktop entries

- `OpenECS --preset paint --install` writes the tool's desktop entry, so the system's launcher lists the tool, and offers it in "Open with" for the file types its `open` function takes. Without `--preset`, it installs the launcher (13.8). `--uninstall` removes the entry.
- The entry is `$XDG_DATA_HOME/applications/<app id>.desktop` (default `~/.local/share`), as the Desktop Entry specification says. It holds:

  | Key              | From the preset                                                                                                |
  | ---------------- | -------------------------------------------------------------------------------------------------------------- |
  | `Name`           | `app.name`                                                                                                     |
  | `Comment`        | `hint`, if it has one                                                                                          |
  | `Icon`           | the full path of `app.icon`, if it has one                                                                     |
  | `Exec`           | the full path of the program, `--preset` with the preset's name, or its full path if a path named it, and `%F` |
  | `StartupWMClass` | `app.id`, so the launcher matches the tool's windows to its entry                                              |
  | `Categories`     | `app.categories`, such as `{ "Graphics" }`                                                                     |
  | `MimeType`       | `app.mimeTypes`, such as `{ "image/png" }`                                                                     |

- A preset named by its name stays found when the program moves; the entry names the program by its full path, so moving the program needs `--install` again.
- On Windows, `--install` writes a shortcut in the Start menu's programs, `<app name>.lnk`, without the characters a file name cannot hold. It runs the program with `--preset` as `Exec` does, starts in the program's folder, and has the hint as its description and `app.id` as its AppUserModelID. The program gives itself the same id when it starts, so the taskbar groups the tool's windows under the shortcut, as SDL's notifications use that id too.
- A PNG `app.icon` becomes the icon file `%LOCALAPPDATA%\OpenECS\icons\<app id>.ico`, which holds the PNG as it is, as shortcuts take no PNG; another kind of image leaves the tool without an icon.
- For "Open with", it registers the class `OpenECS.<app id>` in the user's classes in the registry, whose command opens a file with the tool, and adds the class to the `OpenWithProgids` of each file extension whose content type in the registry is one of `app.mimeTypes`, such as `.jpg` and `.jpeg` for `image/jpeg`.
- `--uninstall` removes the shortcut, the class, its places in "Open with" and the icon file.

## 14. Errors and logging

### 14.1 Error reports

- A native plugin that crashes takes the program down. This cannot be prevented and is accepted.
- An error report holds the plugin, the type of error, a message and, for Lua, a stack trace. It goes to the log. The user sees it where it applies: a faulted panel shows it; a plugin that is missing or fails appears in the log as an error, the program goes on, and the panels of a plugin that did not load show placeholders (4.4). A manifest or preset whose structure is wrong stops the start instead (9.3).
- A message dialog is used only when start-up fails. It names the step, the file it read, the last error that was logged, and where the log file is. The log file opens before the preset is read, so it holds the reason.

### 14.2 Policies

| Situation                                                                             | What happens                                                                                                                                                                                                                                        |
| ------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Manifest unreadable or invalid                                                        | The plugin is skipped and reported, and so are its dependents.                                                                                                                                                                                      |
| Missing dependency or version mismatch                                                | The plugin and its dependents are skipped and reported. The report names what needs the plugin (a plugin, the preset, a session or the user's settings) and, for a missing plugin, each path that was looked in.                                    |
| Dependency cycle                                                                      | The plugins in the cycle are skipped and reported.                                                                                                                                                                                                  |
| Plugin API version mismatch                                                           | The plugin is refused before any of its code runs.                                                                                                                                                                                                  |
| `ECSPlugin_Init` or the plugin's Lua file fails                                       | The plugin is marked failed and its registrations are removed. Plugins that depend on it are skipped and reported.                                                                                                                                  |
| A panel callback raises an error                                                      | The panel becomes a faulted placeholder that shows the error. Its menu has a "Restart" entry (`ecs.panel.restart`), which recreates the panel in place from its last saved state: the state the session last saved, or the one it was created with. |
| A panel's texture or pixels cannot be made                                            | The panel becomes a faulted placeholder, as above, so it is not tried again in every frame. The log has the reason.                                                                                                                                 |
| A plugin callback (timer, event handler) raises an error                              | The error is reported. Repeats of the same error are counted, not reported again.                                                                                                                                                                   |
| A panel's callback, or a popup of the panel, misuses a function (1.4)                 | When the callback returns, the panel becomes a faulted placeholder that shows the misuse, as for a panel callback that raises an error.                                                                                                              |
| A plugin's `Init` or Lua file misuses a function                                      | The plugin fails, as when its `Init` fails.                                                                                                                                                                                                         |
| Any other callback of a plugin misuses a function: `RestoreState`, a timer, an event handler, a setting's `changed`, work in the background or its `done`, a dialog's `done`, a worker, a key's or the preset's open function | The plugin is disabled at the end of the pass, with the plugins that depend on it (9.10). Its panels become placeholders that keep their state and show the misuse.                                                         |
| A plugin's `Destroy` or `SaveState`, a handle type's `Destroy` or a panel's `Destroy` misuses a function | The misuse is reported. The plugin or the panel is going away already.                                                                                                                                                                             |
| Problems while restoring a session                                                    | See 13.3.                                                                                                                                                                                                                                           |

### 14.3 Error convention

- Errors follow 1.4, and services' errors 10.8.
- The core logs the details of each of its own failures, so a result code needs no message.
- A misuse is logged as an error, in the name of the plugin whose callback made it, with what the check says and, when Lua made the call, the Lua stack trace. A test fails on it (17.5).

### 14.4 Logging

- Levels: error, warning, info and debug. Debug builds show every level; other builds show info and above. A message that is not shown is not formatted.
- All logging goes through SDL's log, including SDL's own messages. The core calls it directly; plugins call `ECS_Log`, which adds the plugin's name.
- Each line holds the time, the level, the plugin and the message: `12:30:05.123 warning  [canvas] message`. The core's own lines name `ecs`.
- Lines go to standard error and to the log file (16). Each start writes the log file anew.
- On a terminal, the level shows in a colour: the detailed levels dim, info green, warning yellow, error red and critical bold red. The log file, pipes and the setting `NO_COLOR` (no-color.org) get plain text.
- The core emits each line it writes as the event `ecs.logged` (8.4), so a plugin, such as a console, shows the log to the tool's user; C and Lua subscribe alike. The message has no time, level or plugin: those are the value's fields, and `plugin` is `ecs` for the core's and SDL's lines. Lines of other threads wait in a queue of 512 lines for the main thread, which they wake; the event brings them in the order they were logged, at the start of the next pass that delivers events. When the queue is full, the lines after it are left out of the event, not of the log, and one warning line says how many.
- A line logged while a handler of `ecs.logged` runs is written, but not emitted again, so a handler that logs, or calls what logs, cannot loop. Lines of other threads logged meanwhile are emitted.
- On Windows, only Debug builds open a console window. Other builds are Windows GUI programs, so a release opens no console beside its window. Started from a console, such as `cmd.exe`, they write to it; started with their output sent elsewhere, such as into a pipe, they write there.

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
| The user's settings             | `$XDG_CONFIG_HOME/OpenECS/settings.lua` (default `~/.config`)             |
| The user's presets              | `$XDG_CONFIG_HOME/OpenECS/presets/`                                       |
| The user's plugins              | `$XDG_DATA_HOME/OpenECS/plugins/` (default `~/.local/share`)              |
| Saved sessions (default folder) | `$XDG_DATA_HOME/OpenECS/sessions/`                                        |
| A tool's last session           | `$XDG_STATE_HOME/OpenECS/<app id>/session.lua` (default `~/.local/state`) |
| The log                         | `$XDG_STATE_HOME/OpenECS/OpenECS.log`                                     |
| First-party plugins and presets | `plugins/` and `presets/` next to the executable                          |
| Examples                        | `examples/` next to the executable                                        |
| The core's settings             | `resources/settings.lua` next to the executable                           |

- On Linux, the core reads `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `XDG_STATE_HOME` itself, as SDL has no function for the configuration and state folders.
- On Windows, every one of these folders is SDL's pref path, `%APPDATA%\OpenECS\`, as Windows has no XDG folders. What they hold has different names, so they share it.

## 17. Build and dependencies

### 17.1 Dependencies

| Library                     | Status                                                |
| --------------------------- | ----------------------------------------------------- |
| SDL3                        | core                                                  |
| SDL3_ttf                    | core: text in the core's interface                    |
| Clay                        | core: the core's own interface                        |
| LuaJIT                      | core: Lua                                             |
| lua-compat-5.3              | core: the C API's Lua 5.3 forms, and `utf8` (9.6)     |
| libffi                      | core: calls by signature                              |
| shu                         | core: basic types and results                         |
| stb (`stb_ds.h`)            | core: dynamic arrays and hash maps                    |
| JetBrains Mono              | core: the font of the interface, `resources/JetBrainsMono-Regular.ttf` from JetBrains' release 2.304, under the SIL Open Font License 1.1 in `resources/JetBrainsMono-LICENSE.txt`; draw's monospace font too (OpenECS-std DESIGN 2) |
| Roboto                      | core: `resources/Roboto-Regular.ttf`, a proportional font that `ecs.font` can name, under the Apache License 2.0 in `resources/Roboto-LICENSE.txt` |
| SDL3_image                  | not used; the core loads PNG files with `SDL_LoadPNG` |
| SDL3_mixer, SDL3_net, cgltf | not core; plugins may compile them in (20.1)          |

### 17.2 Linking

- SDL3 and SDL3_ttf are shared libraries shipped next to the executable. On Linux it finds them through an `$ORIGIN` run path; Windows finds a program's DLLs in its own folder. So the core and every plugin share one copy of SDL.
- LuaJIT, lua-compat-5.3, Clay, libffi and stb are linked statically into the executable.
- LuaJIT is built as its `src/Makefile` builds it, and writes what it generates into the build's folder: the build compiles its host tools `minilua` and `buildvm`, reads its architecture from its `lj_arch.h`, makes the virtual machine with DynASM and `buildvm`, and compiles `ljamalg.c` with `LUAJIT_ENABLE_LUA52COMPAT`. Debug builds add LuaJIT's checks of the C API and its assertions. Errors unwind with the system's unwinder, through the C frames between a protected call and the error. The modules of `jit/`, such as `jit.p`, go next to the executable.
- lua-compat-5.3's C functions and its `utf8` library are compiled into LuaJIT's library under their own prefix. The core reaches Lua through lua-compat-5.3's header, which gives it the C API of Lua 5.3, such as `lua_getfield` that gives the type.
- libffi is compiled without its configure script. Its configuration is a glue header for Linux on x86_64 and aarch64 and Windows on x86_64, `dependencies/other/libffi/fficonfig.h`, and the build makes `ffi.h` from libffi's template.
- The executable exports the functions marked `OPENECS_EXPORT` and nothing else. The core is compiled with hidden symbols by default, and with `OPENECS_CORE` defined, so `OPENECS_EXPORT` exports.
- The build writes what people use into `build/<type>/`: the program, its plugins, presets and resources in `bin/`, the plugin interface in `include/` (10.10), and on Windows OpenECS's import library in `lib/`. What only the build uses, such as the dependencies' headers and libraries, stays in `.shu/<type>/`.
- Native plugins are shared libraries, built against `include/` only. On Linux they do not link against the core; their calls to it are resolved when they are loaded. A DLL on Windows cannot call into the executable without its import library, so there the executable writes `lib/libOpenECS.a`, and plugins link it.
- The standard plugins ship with OpenECS, so they reuse two of its built dependencies from `.shu/<type>/dependencies/` (17.7): SDL3 and SDL3_ttf, the platform the executable loads. Every other library they use, such as stb_ds, comes from OpenECS-std and is compiled into the plugin. Like their calls to the core, their calls to SDL are resolved when the plugin is loaded, against the libraries the executable loaded, so they use the executable's copy of SDL. On Windows they link SDL's import libraries for it. No other plugin sees OpenECS's dependencies: neither their headers nor their import libraries are in `include/` or `lib/`, and a plugin that uses a library brings it itself.
- A plugin's own functions that the core calls, such as `ECSPlugin_Init`, are marked `OPENECS_PLUGIN`, which exports them from the plugin's library.
- A first-party plugin is built from every C file in its folder, so the build needs no settings for each plugin. Its Lua files and its folders, which hold its Lua modules (9.6), are copied.
- OpenECS-std and OpenECS-examples build their plugins this way, with their own builds (17.7).

### 17.3 Compiler

- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.
- Debug builds of the core and of plugins add the static analyzer (`-fanalyzer`) and the address, leak and undefined-behaviour sanitizers (`-fsanitize=address,undefined`). Dependencies get neither.
- The Sanitizers module sets the sanitizers' options and hides leaks inside the system libraries that SDL loads: graphics drivers, display servers, input methods, audio systems and D-Bus. In other builds its functions do nothing.
- Debug builds keep every library loaded until the program exits, so a leak inside one is matched by its name: the executable's `dlclose` does nothing, and comes before the system's. Graphics drivers are otherwise unloaded when SDL or Vulkan's loader is done with them, and their leaks would show an unknown module.
- Debug builds check for leaks at shutdown, after OpenECS has freed its memory and before `SDL_Quit`.
- Release builds set `SDL_ASSERT_LEVEL` to 0, so they have no assertions. Debug builds set it to 2.

### 17.4 Dependency versions

- Every dependency is a git submodule at a fixed commit: the latest commit of its development branch, or of a release tag when the branch lacks files that its release makes. LuaJIT's is its branch `v2.1`, from which it releases; its release number is the time of that commit. An update moves each submodule to its newest such commit, and passes the checks (19.3) like any change.
- SDL_ttf brings FreeType, HarfBuzz and PlutoSVG as its own submodules, at the commits SDL_ttf names.

### 17.5 Tests

- The examples are the tests (17.6). A test is a Lua file of an example whose name starts with `test`. Every build runs it with `OpenECS --test FILE`: a Debug build checks memory with the sanitizers, and a Release build runs at the speed users get.
- The file returns a table: `preset`, the preset to start from (a name, or a path relative to the test file; `launcher` if left out), `files`, paths relative to the test file that are opened as if the command line named them (13.6), `presets` and `sessions`, folders relative to the test file that stand for the user's presets and the saved sessions (16), and `run`, a function that gets the `test` table.
- The examples teach plugin authors and check that the core and the standard plugins work, with one set of files. A feature of the core or of a standard plugin is checked by the test of the example that shows it.
- Tests run from the build's copy of the examples, `bin/examples/`, so a test reads the build's copies of the plugin header, `ecs.lua` and the core's settings file.
- A test starts from its preset alone. It loads only the plugins its preset names, not those that the settings files name for every tool (12.3). It reads none of the user's settings, plugins or sessions, and writes no session and no log file. SDL's offscreen video driver, software renderer and dummy audio driver are the defaults, so a test needs no display and makes no sound. It sees no real gamepad, only the virtual ones it plugs in; the variables `SDL_VIDEO_DRIVER`, `SDL_RENDER_DRIVER` and `SDL_AUDIO_DRIVER` choose others, for example to watch a test.
- `run` is a coroutine in the main loop. A function that sends input or waits pauses it. Each input event gets its own pass of the loop, and `run` goes on when the last one is handled, its events are delivered and the window is drawn. While a test runs, frames are not paced (3.1).
- The `test` table:

  | Function                                                             | Does                                                                                                                                                                                                                                               |
  | -------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
  | `key(combination)`                                                   | Presses and releases a key combination, such as `"Alt+W"` (7.3).                                                                                                                                                                                   |
  | `move(x, y)`                                                         | Moves the pointer. Positions are in layout units of the OS window that `window` chose.                                                                                                                                                             |
  | `press(x, y, button, modifiers)`, `release(x, y, button, modifiers)` | Presses or releases a button: 1 left (the default), 2 middle, 3 right. The `modifiers`, such as `"Shift"`, are held during it.                                                                                                                     |
  | `click(x, y, button, modifiers)`                                     | Presses and releases a button.                                                                                                                                                                                                                     |
  | `drag(x, y, toX, toY, modifiers)`                                    | Presses the left button, moves in steps and releases it.                                                                                                                                                                                           |
  | `wheel(x, y, amount)`                                                | Turns the wheel; a positive amount is away from the user.                                                                                                                                                                                          |
  | `text(text)`                                                         | Types text to the focused panel, as an input method gives it (4.6).                                                                                                                                                                                |
  | `dropFiles(x, y, paths)`, `dropText(x, y, text)`                     | Drops files or text from another application at a position (7.9).                                                                                                                                                                                  |
  | `window(number)`                                                     | Chooses the OS window that later input goes to, and that `screenshot` saves: 1 for the main window, then the current workspace's pop-out windows in the order they were made. It is 1 at start. Gives the window's position on the screen, `x, y`. |
  | `close()`                                                            | Asks the chosen OS window to close, as its close button does.                                                                                                                                                                                      |
  | `call(name, ...)`                                                    | Runs a bound function (7.8), such as `"ecs.layout.maximize"`, as a key would. With values, runs any function with them, as a Lua plugin calls it (10.4), and gives its results, such as `test.call("tally.addTo", "clicks", 4)`. |
  | `keyEvent(combination, down, ms, repeat)`                            | Sends one press or release of a key without modifiers as SDL gives it, with its time in milliseconds and whether it is SDL's repeat, such as an input method's own repeat (4.6).                                                                   |
  | `wait(seconds)`                                                      | Lets the program run, for timers. Without seconds, it waits one pass of the loop.                                                                                                                                                                  |
  | `session()`                                                          | The session that quitting would save now, as a Lua table (13.2).                                                                                                                                                                                   |
  | `rect(id)`                                                           | The rectangle of the shown panel with that id: `x`, `y`, `width` and `height`, and `window`, the number of its OS window.                                                                                                                          |
  | `panel(id)`                                                          | The panel with that id: its `title`, `type`, `unsaved` mark, `state`, its saved state as the session would save it, and `fault`, the error that stopped it or a missing type.                                                                      |
  | `panels()`                                                           | Every panel, by workspace, OS window and place in the layout tree: its `id`, `type` and `title`, and the numbers of its `workspace` and `window`.                                                                                                  |
  | `setting(name)`                                                      | A setting's value in effect (12), such as a colour of the theme.                                                                                                                                                                                   |
  | `setSetting(name, value)`                                            | Changes a setting as the user's settings file does (12.2), and goes on once its owner has heard of it; nil takes the user's value away.                                                                                                           |
  | `screenshot(path)`                                                   | Draws a frame and saves the chosen OS window's picture as a PNG file.                                                                                                                                                                              |
  | `pixel(x, y)`                                                        | Draws a frame and gives the colour that the chosen OS window shows at a position in its layout units, as `"#RRGGBB"`, such as a GPU panel's (5.4).                                                                                                 |
  | `popup(number)`                                                      | An open popup, the oldest first: its rectangle `x`, `y`, `width` and `height` in its OS window, and `window`, the number of that window (4.6).                                                                                                     |
  | `titleBar()`                                                         | Draws a frame and gives what the chosen OS window's title bar shows (6.11), in order: each item's `kind` (`"name"`, `"menu"`, `"button"`, `"workspace"`, the workspaces' menu, or `"window"`, a window button: `"minimize"`, `"maximize"` or `"restore"`, and `"close"`), `label` and rectangle `rect`, and whether it is `on` and `enabled`. Items cut for a narrow window are left out.                                  |
  | `menu()`                                                             | The entries of the open menu that takes the keys, or nil: each one's `label`, `keys` if it shows any, and whether it is `enabled` and `chosen`. The core's menus (6.8) come first; a popup menu gives the entries it set with `setItems` (4.6). A heading of the core's menus is not enabled, and an entry that opens a submenu has no keys. |
  | `dialog()`                                                           | The message dialog that waits for an answer (7.7), or nil: its `title`, `message` and `buttons`, the number of the `selected` button, and `rects`, each button's rectangle in its OS window. A test answers it with `key` or `click`.              |
  | `fileDialog(paths)`                                                  | The oldest file dialog that waits for an answer (7.7), or nil: its `type`, `filters`, `location` and `many`, as `ecs.dialog.show` takes them. With a list of paths, it answers the dialog with them, as the user does, and goes on once the dialog's `Done` function has them; an empty list cancels it. |
  | `errors()`                                                           | The errors that plugins reported (14.2) since the program started, each once and in order, as `"plugin: message"`.                                                                                                                               |
  | `misuses()`                                                         | Takes the misuses (1.4) since the last call, so the test does not fail on them, and gives what each check says. A test that checks a misuse reads it so.                                                                                         |
  | `match(actual, expected, message)`                                   | Checks that `actual` has the same number of list items as `expected`, and each item and field it names.                                                                                                                                            |

- Before `run` goes on, each OS window gets the size the system gives it, as a tiling compositor does, and the layout follows it.
- A pointer event over a shown popup window goes to that popup, in its own positions, as the system gives it, unless it is a tooltip. While a button is held, the events go to the window that got the press.
- `match` leaves out fields that `expected` does not name, so a test checks only what it is about. A difference raises an error that names its path, such as `workspaces[1].windows[1].panels`.
- A Lua error fails the test and logs it with its stack trace. So does a misuse (1.4) by any plugin that `misuses` did not take, while the test goes on to its end. When `run` returns, the program quits without asking about unsaved work.
- While a message dialog waits (7.7), the test goes on, so it can read and answer the dialog. A dialog that the test's own step opens, as `call` can, cannot wait for the test, so it is answered with its last button, and so is one that waits when the test ends.
- The exit status is 0 when the test passes, and not 0 when it fails or cannot start.
- OpenECS's build runs the tests with `-t`, `--test`: it builds with the examples, in Debug unless `-b` names another type, then runs every test in `bin/examples/`, or only the tests of the examples named after it, such as `-t c12_sketch`. It prints `pass` or `FAIL` for each test, and fails when one fails. A test fails when the program fails, or when its output has a sanitizer's report: `Sanitizer` or `runtime error:`.
- The checks (19.3) also run them on Wayland, in a headless sway that tiles the windows. There, popups and pop-out windows are windows of their own.

### 17.6 Examples

- The examples are in OpenECS-examples (17.7). An example is a folder that shows plugin authors one part of the plugin interface, or one standard plugin. Its name orders the examples from the first steps to the complex ones, so they are read in order: the examples of the plugin interface start with `c` and come first, `c1_hello`, `c2_native` and so on, then those of the standard plugins start with `s`, `s1_terminal` and so on.
- An example holds a preset, `preset.lua`, the folders of its plugins, and its tests, the files whose names start with `test`. The preset names the folder in `pluginsDir`, so the example needs nothing else but the standard plugins.
- Its comments explain how the core behaves: what a statement makes the core do, when it may be called, and a concept the first time it appears. They do not explain the example's own logic or repeat the functions' documentation.
- With the flag `--examples`, the examples' build copies the examples' folders of `examples/`, sources too, to `bin/examples/`, and builds each example's native plugins there. A native plugin includes `OpenECS.h` and the standard plugins' definition headers from the build's `include/` alone (10.10), as any plugin's author does.
- A change that breaks an example changes the example in the same piece of work, so every test passes.

### 17.7 Repositories

- OpenECS holds the core. Two more repositories hold what ships with it:

  | Repository         | Holds                                                                                                                      | Folder in OpenECS |
  | ------------------ | -------------------------------------------------------------------------------------------------------------------------- | ----------------- |
  | `OpenECS-std`      | The standard plugins, the first-party plugins `settings`, `launcher` and `panels`, the preset `launcher`, and their design | `std/`            |
  | `OpenECS-examples` | The examples (17.6)                                                                                                        | `examples/`       |

- Both are git submodules of OpenECS, so one build and one release hold all three (19.4). A submodule names one commit of its repository's `dev`; OpenECS moves it forward in a pull request.
- Each repository has a build of its own, `shuild.c`, which OpenECS's build runs. OpenECS's build compiles it with shu and shuild from OpenECS's `dependencies/` into `shuild.ignore` in the repository's folder, because shuild finds every path from the folder of the program it runs in. It runs it there with the build type and OpenECS's build folder as seen from that folder, and OpenECS-std's also with the folder of OpenECS's built dependencies, `.shu/<type>/dependencies/`, with `-d` (17.2).
- OpenECS's build builds the standard plugins unless it is given `--no-std`, and the examples when it is given `--examples`:

  | Flag of OpenECS's build | Runs                                                                                            |
  | ----------------------- | ----------------------------------------------------------------------------------------------- |
  | (none)                  | `std/shuild.c`: the standard plugins into `bin/plugins/`, and their presets into `bin/presets/` |
  | `-n`, `--no-std`        | not `std/shuild.c`, so the build holds the core alone                                           |
  | `-e`, `--examples`      | `examples/shuild.c`: the examples into `bin/examples/`                                          |
  | `-t`, `--test`          | a build with the examples, then the examples' tests, or the named examples' (17.5)              |

- A repository that is not checked out is skipped with a warning.
- Each repository has its own checks (19.3). They check out OpenECS's `dev` with its submodules, put the repository's own commit in its folder, build OpenECS with it and the examples, and run the examples' tests.
- Each repository follows OpenECS's conventions and rules for contributors; its `CLAUDE.md` and `DESIGN.md` name what is its own.

## 18. Platform notes

The core provides the clipboard (text and other typed data), drag and drop with other applications, file and message dialogs (7.6, 7.7, 7.9), and adds a tool to the system's application launcher, with the file types it opens (13.9). Plugins provide a tray icon and desktop notifications.

- OpenECS supports X11 and Wayland, through SDL3.
- OpenECS runs on x86_64 and aarch64 processors (17.2).
- On Wayland, a panel dropped outside its window lands where the pointer enters another of the program's windows, and its drop place is not marked there (6.10). The position of a new window cannot be chosen.
- Some Wayland desktops draw no window frames; OpenECS draws its own (6.11).
- Popups are positioned relative to their parent window, which Wayland supports.

## 19. Versions and releases

### 19.1 Versions

- OpenECS uses semantic versioning. Its version is `OPENECS_VERSION` in `OpenECS.h`, and `OpenECS --version` prints it (13.6).
- From 1.0.0, a major release breaks something that worked: the plugin interface, the `ecs` module, the names of the core's functions, settings and events, or the files that presets, sessions and settings are read from. A minor release only adds. A patch release only fixes.
- Before 1.0.0, a minor release may also break those things, and a patch release only fixes.
- Between releases, `OPENECS_VERSION` is the next release with `-dev`, such as `0.2.0-dev`. A release candidate ends with `-rc.1`, `-rc.2` and so on.
- Two numbers change on their own: `OPENECS_API_VERSION` when native plugins must be built again (9.5), and the `format` of presets and sessions (13.2) when an older OpenECS cannot read the new files.
- A release is a tag `vMAJOR.MINOR.PATCH`, such as `v0.1.0`, on `main`.

### 19.2 Branches

- `main` holds the releases. It changes only through a pull request from `dev`, or from a branch that fixes a release.
- `dev` is the default branch. Every change reaches it through a pull request from a branch that started from `dev`: `feature/<feature>`, `fix/<bug>`, `refactor/<area>` or `docs/<area>`.
- A fix for a release starts from its tag. It is merged into `main` and released as a patch, then merged into `dev` too.
- Pull requests are merged with a merge commit, never squashed or rebased, so `main` and `dev` share one history.
- The rulesets in `.github/rulesets/` hold these rules; they are imported in the repository's settings. `main` and `dev` cannot be deleted or force-pushed, and a pull request into them needs the checks (19.3) to pass and its conversations resolved. Repository admins may skip the checks through a pull request. Only repository admins create, move or delete release tags.

### 19.3 Checks

- `.github/workflows/checks.yml` runs on every push and pull request to `dev` and `main`:

  | Check             | Does                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
  | ----------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
  | `Format`          | Checks that clang-format leaves the core's C files as they are (1.6).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
  | `Debug (Linux)`   | Builds and runs shuild's example `16_compiler_detection`, so shuild's compiler detection runs; it fails when shuild finds no compiler for the system. Then builds in Debug, which runs the static analyzer (17.3); an error or warning in OpenECS's own files fails it. Then runs the examples' tests (17.5) under the sanitizers, on SDL's offscreen driver and then on Wayland; a failed test or a report fails it. Then adds a tool to the launcher with `--install` and removes it with `--uninstall` (13.9), and checks what each writes and removes. Then writes the definition files of the plugins of the example `c5_services` (10.10) and compiles each C header, and checks that every function of the standard plugins says what each parameter and its result are (10.1). |
  | `Release (Linux)` | Builds in Release, so code that only Release builds compile or link fails before a release. It runs only for a release.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
  | `Windows`         | Runs shuild's compiler detection, as `Debug (Linux)` does. Then builds on Windows with LLVM-MinGW: in Debug, then runs the examples' tests, installs and uninstalls a tool, and writes the definition files, as `Debug (Linux)` does; then in Release, only for a release. It has no static analyzer or sanitizers, which MinGW lacks.                                                                                                                                                                                                                                                                                                                                                                 |
  | `Lua (LuaLS)`     | Checks every Lua file with LuaLS, so code that misuses the `ecs` module, and mistakes in `ecs.lua`, fail it (9.6).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |

- The steps are scripts in `.github/scripts/`, for CI. Each builds or tests with shuild, so a contributor runs the same build and tests with `shuild.c`: `-b debug -e`, `-b release -e` and `-t`.
- A new push to a branch or a pull request cancels its older run of the checks.
- The checks for a release are those on a pull request into `main`, a push to `main`, or a run by hand. The others leave out the Release builds, so the checks of each change finish sooner. A skipped check counts as passed.
- The built dependencies, and the C++ libraries that std builds with CMake, are kept between runs until a submodule of `dependencies/` or the build changes. OpenECS's own files are compiled every time, so the analyzer sees all of them.

### 19.4 Releases

- OpenECS, OpenECS-std and OpenECS-examples are released together, with the same version. Each has a `dev` branch, a `main` branch that holds its releases, and a tag `vVERSION` for each release on its `main`.
- `.github/workflows/release.yml` runs when a version tag is pushed. It fails when the tag does not match `OPENECS_VERSION`.
- For each platform, it makes a Release build and packs it into `openecs-VERSION-PLATFORM.tar.gz`: the program with its plugins, presets, examples and resources, `include/` with the plugin interface and the standard plugins' definition files (10.10), `LICENSE`, and the licenses of the works it includes. The Windows package is a zip file, which Windows opens itself, with SDL's DLLs beside the program. Its `lib/` holds OpenECS's import library, which a C plugin links on Windows (17.2).
- It packs the source with every submodule into `openecs-VERSION-source.tar.gz`, because GitHub's own source archives leave the submodules out. `SHA256SUMS` holds the checksums of the archives.
- It makes a draft release with these files. Its description is `.github/release-notes/vVERSION.md`. A version below 1.0.0, or one with a suffix, is marked as a pre-release.
- OpenECS-std and OpenECS-examples make a draft release of their own from their tag, with their source and its submodules, and their own description. For each platform, it also holds their part of OpenECS's Release build of the same version, `openecs-std-VERSION-PLATFORM` and `openecs-examples-VERSION-PLATFORM`: std's plugins, the preset `launcher` and the plugins' definition files, or the examples with their native plugins. Unpacked into an OpenECS folder of that version, their folders join OpenECS's. Their workflows check out OpenECS at the same tag, so OpenECS's tag is pushed first.
- Releases are made for Linux and Windows on x86_64. The workflow also lists the other platforms, commented out. Linux is the priority; each platform needs upkeep, so others are added only when they cost little. Linux on aarch64 waits for the first stable release, 1.0.0.
- Linux builds are made on Ubuntu 24.04, so they need glibc 2.38 or later. Windows builds are made on Windows Server 2025 with LLVM-MinGW's UCRT runtime, which Windows 10 and later have.
- The steps of a release:
  1. In OpenECS-std and OpenECS-examples, write each release's description, and merge `dev` into `main` with a pull request.
  2. On OpenECS's `dev`, move `std/` and `examples/` to those `main` commits, set `OPENECS_VERSION` to the release, and write its description.
  3. Merge OpenECS's `dev` into `main` with a pull request.
  4. Tag the three merge commits `vVERSION` and push the tags, OpenECS's first.
  5. Check the three draft releases, then publish them.
  6. On OpenECS's `dev`, set `OPENECS_VERSION` to the next version with `-dev`.

### 19.5 Release descriptions

- They use plain words and short sentences, for users and plugin authors.
- They say what is new, and what people can do with it.
- They say what breaks after the release: what plugins, presets, settings and sessions must change, and how.
- They name the Lua runtime or a library for plugins that changed (9.6), with its new version.
- They name the small fixes briefly.
- Each repository's description says what changed in that repository. OpenECS's names the others' releases and links to them, so each change is explained in one place.

## 20. Standard plugins

Some plugins are made and shipped together with OpenECS, as the product needs them. They are plugins like any other: they use only the plugin API, the core neither knows nor favours them, and a user can replace any of them with a plugin of the same name.

- Standard plugins, whose services other plugins build on: **draw** draws shapes, lines, triangles, images and text; **ui** gives user-interface elements with layout, such as buttons, sliders, lists and trees; **textgrid** grids of characters for terminals and logs; **image** and **audio** read image and sound files for draw and mixer; **mixer** plays sounds; **net** and **web** give sockets and HTTP that never block; **gltf** and **cglm** serve 3D scenes; **fs** lists and makes folders; and more, which OpenECS-std's DESIGN.md lists.
- Panels and windows built on ui: **files**, **viewer** and **console** for a tool's files and output; the **settings** window, the **keys** list, the **panels** list, the **launcher** and the **profiler**.

### 20.1 Rules

- A standard plugin is a first-party plugin whose services other plugins build on. It is built and loaded like a third-party plugin: from its folder in OpenECS-std (17.7), against the plugin interface only (2.2), with a manifest and a version of its own.
- The core's code never refers to a standard plugin, and a user's plugin of the same name replaces it (9.3, 9.9). The core's settings file names the plugins that every tool loads (12.3), and they bind their own keys and menu entries.
- Plugins that use a standard plugin name it in their manifest's `depends`, and look up its functions with the signatures it documents (10.5).
- Each standard plugin has one job, and no two share one. A plugin that runs something, such as draw, which draws, or mixer, which plays sound, is an executor: it knows its primitives. A plugin that reads a file format is a loader: it turns the file into the objects of an executor, as image gives draw's images and audio gives mixer's sounds. A loader may also write those objects back to the format, as image writes draw's images. So a format is handled in one place, and what it gives works wherever the executor does.
- Standard plugins are generic building blocks. Views of one domain, such as a scene's hierarchy or an inspector, belong to the plugins of tools, which build them from the blocks.
- Platform-specific code stays inside the plugins that need it, behind their functions, so porting OpenECS to another system changes those plugins and not the plugins built on them.
- A standard plugin needs nothing but OpenECS: it links no library. Its calls to SDL and SDL_ttf are resolved against the executable's copies (17.2), and any other library it uses is compiled into it. So copying its folder into a `plugins/` folder installs it.
- A plugin never uses another plugin's libraries (0.6). Two plugins that use one library, such as zlib, each compile their own copy into themselves, and its functions stay hidden inside each. A library that needs another, as libcurl needs zlib for compressed answers, gets it compiled into the same plugin.

### 20.2 OpenECS-std

The standard plugins are in OpenECS-std (17.7). Their design is in [OpenECS-std DESIGN.md](https://github.com/omerfuyar/OpenECS-std/blob/dev/DESIGN.md).

---

## Glossary

**ABI (application binary interface).** The rules for how compiled code fits together: where each field of a structure sits in memory, how large each type is, how functions receive arguments. Code that disagrees about the ABI breaks, even though no compiler complained.

**Action.** A function that makes something happen, such as closing a panel.

**Allman style.** A brace style in which each brace is on its own line.

**AltGr.** A key that many keyboard layouts use to type extra characters, such as @ or {.

**API (application programming interface).** The functions and types that one piece of code offers to another.

**Assertion.** A check of a condition that must always hold. If it fails, the program logs where and stops.

**Binding plugin.** A plugin whose job is to offer a library to other plugins.

**Buffer.** A pointer to memory together with its length.

**Callback.** A function passed to other code so that it can be called later.

**Cdata (LuaJIT).** A value of LuaJIT's FFI that holds C data, such as a number, a pointer or a struct, managed by Lua's garbage collector. Handles are cdata.

**Clay.** A layout library. It computes the sizes and positions of rectangles.

**Closure (libffi).** A small piece of code that libffi generates at run time. It looks like an ordinary C function, but forwards each call to a handler, which here calls a Lua function.

**Compositor.** On Wayland, the program that draws all windows on screen and decides where they go and which one has focus.

**Continuous drawing.** A panel type's request to be drawn every frame while it is visible.

**Core.** OpenECS without plugins.

**Core prefix.** The one key combination reserved for the core. The key pressed after it chooses a core action.

**Coroutine (Lua).** A Lua function that can pause and later go on from where it paused.

**Dependency.** Something that a piece of software needs in order to work.

**Desktop entry.** A `.desktop` file that tells Linux desktops about a program: its name, icon, command and the file types it opens. Application launchers list the programs that have one.

**Destructor.** A function that releases an object when it is no longer needed.

**Docking.** Attaching a panel to a place in the layout, by splitting or grouping.

**Environment (Lua).** The table of global names that a piece of Lua code sees.

**Event.** A notification that something happened.

**Executable.** A program that can be run directly.

**Executor.** A plugin that runs something with its own primitives, such as drawing or playing sound, for other plugins.

**FFI (LuaJIT).** LuaJIT's library that declares C types, makes C data and calls C functions from Lua code, which the JIT compiles as C calls.

**First-party plugin.** A plugin made and shipped together with OpenECS.

**Focus.** The one panel that receives keyboard input.

**Frame.** One pass of drawing and presenting.

**Function pointer.** A value that holds the address of a function, so the function can be called through it.

**glibc.** The C standard library of most Linux systems. A program built against a newer glibc does not start on a system with an older one.

**GPU.** The graphics card.

**GPU device.** SDL's object for the graphics card. Textures and other GPU resources belong to a device.

**Grip.** A small handle that the core shows on a panel without a tab row, used to move the panel or open its menu.

**Group.** One or more panels that share one place in the layout; one of them is shown at a time.

**Handle.** A token that stands for an object owned by someone else, so it can be passed around without exposing its inside.

**Hit testing.** Finding which element is under a point, such as the pointer.

**Input method.** Software that composes characters a keyboard cannot type directly, for languages such as Chinese or Japanese.

**JIT (just-in-time compiler).** LuaJIT's compiler, which turns the Lua code that runs often into machine code while the program runs. It follows the code's path, a trace, through calls; a call through the C API ends a trace, and a call through the FFI does not.

**Keybinding.** A link between a key combination and a function, written in a keys table (7.2).

**Launcher.** The tool that OpenECS shows when it starts without a preset. It lists presets and saved sessions, and opens the one the user chooses.

**Layout tree.** The splits, groups and panels inside one OS window.

**Layout unit.** The unit of positions and sizes in the layout and in a panel. One takes `ecs.scale` pixels in a panel, a popup and the sizes that the core's settings give, such as `ecs.tabRowHeight`, and a pixel in the layout's positions, such as a split's fixed size (5.1).

**Least authority.** The rule that a plugin can do only what its job needs.

**libffi.** A library that calls a C function when only its signature is known at run time.

**Loader.** A plugin that reads a file format into the objects of an executor, and may write them back.

**Local name.** The part of a name after the plugin's name and a dot, such as `view` in `canvas.view`.

**Lua.** A small scripting language, used for plugins and for presets, sessions, settings and manifests.

**LuaJIT.** A Lua 5.1 that compiles Lua code to machine code while it runs, with extensions of its own, such as the FFI (9.6).

**LuaLS (Lua language server).** The program behind Lua support in editors such as VS Code. It reads annotations in comments, such as `---@param`, to give completion, documentation and warnings.

**Main thread.** The thread on which the program starts. OS windows, input and the Lua code of plugins run there.

**Manifest.** A small file next to a plugin that gives its name, version and dependencies.

**Merge commit.** A commit that joins two branches and keeps the commits of both.

**Metatable (Lua).** A table that defines how a Lua value behaves. For handles, it carries the type name.

**MIME type.** A name for a kind of file, such as `image/png`. Desktops use it to choose the programs that open a file.

**Misuse.** A call that breaks the rules of the function it calls, such as one with a wrong argument. It is a bug of the caller, not of the function.

**Modal editing.** Editing in which the same key does different things depending on the current mode.

**Native plugin.** A plugin made of compiled C code in a shared library.

**Offscreen renderer.** A 2D renderer that draws into a texture instead of a window.

**Offscreen video driver.** An SDL video driver that draws OS windows into memory instead of onto a display.

**Opaque handle.** A pointer to a structure whose fields are hidden, so only the functions that own it can change it.

**OS window.** A window of the operating system, with its own title bar. It holds one layout tree.

**Panel.** A rectangular area of the layout whose contents come from a plugin.

**Panel type.** What a kind of panel shows and does. Many panels can share one type.

**Placeholder.** A stand-in panel that the core shows when a panel's type is missing or has failed. It keeps the panel's saved state.

**Plugin.** An add-on module that provides panel types, services, events or settings.

**Plugin API.** The functions and types through which plugins use the core.

**pop out.** Moving a panel into a new OS window.

**Popup.** A small, temporary window, such as a menu or a tooltip, that belongs to a panel.

**Premultiplied alpha.** A way of storing transparent pixels in which the colour is already multiplied by the opacity. Blending is faster and has no edge artifacts.

**Preset.** A hand-written file that describes a tool.

**Protected call (Lua).** A way to call Lua code so that an error is caught and returned, instead of jumping out through the caller.

**Pull request.** A request on GitHub to merge one branch into another, where the change is checked and discussed first.

**Register.** To tell OpenECS, by calling one of its functions, that something exists.

**Registry (Lua).** A table that Lua keeps for C code to store values it needs to keep alive.

**Root.** The top of a layout tree. There is one per OS window.

**Ruleset (GitHub).** A set of rules that GitHub enforces for branches or tags, such as requiring pull requests.

**Run path.** A directory written into a program where it looks for shared libraries. `$ORIGIN` means the program's own directory.

**Sanitizer.** A compiler option that adds checks for memory errors and undefined behaviour while the program runs.

**Scale.** How many screen pixels make one unit of the layout.

**SDL.** A library for OS windows, input, graphics and other system features.

**SDL3_ttf.** SDL's library for drawing text.

**Semantic versioning.** Version numbers of the form MAJOR.MINOR.PATCH, where a new major number means an incompatible change.

**Service.** A named set of functions that a plugin offers to other plugins and scripts.

**Session.** The saved state of OpenECS: plugins, workspaces, panels and their state.

**Setting.** A named, typed value that configures the core or a plugin.

**Shared library.** A compiled code file that a program can load while it runs (`.so` on Linux).

**shu.h.** A header of basic types (`i32`, `u32`, `usz`, `SHUSlice`, `SHUResult`) and macros, shared by the core and plugins.

**SHUResult.** shu's result code. `SHUResult_Ok` (zero) means success; every other value names an error.

**SHUSlice.** A pointer and a size that describe a piece of memory, passed together.

**SHUWUR.** A `shu.h` attribute macro that makes the compiler warn when a function's result is not used.

**Signature.** A short description of what a function takes and returns.

**Split.** A layout node that divides an area horizontally or vertically.

**Standard plugin.** A first-party plugin whose services other plugins build on, such as draw and ui.

**Static analyzer.** A compiler pass that follows the paths through the code and warns about errors it finds, such as a null pointer that is dereferenced, without running the program.

**Struct (structure).** A group of named fields stored together in memory.

**Submodule (git).** Another repository included at a fixed version inside a repository.

**Surface.** The picture a panel draws into.

**Symbol.** A name in a compiled file that other code can look up, such as an exported function.

**Tab row.** The row of tabs that a group with two or more panels shows.

**Tag (git).** A name for one commit, such as `v0.1.0`.

**Texture.** An image stored on the graphics card.

**Theme.** A named set of settings, such as the colours of a dark or a light interface.

**Timer.** A request to run a function after some time, once or repeatedly.

**Title bar.** The strip along the top of a window that shows its name and the buttons that minimize, maximize and close it.

**Topological order.** An order in which every item comes after the items it depends on.

**Unsaved work.** A panel's mark that it has changes that are not saved yet.

**Userdata (Lua).** A block of memory managed by Lua's garbage collector that holds data from C, such as a timer's or a worker's handle in Lua.

**Vsync.** Waiting for the display's refresh before showing a new frame, so drawing matches the screen's rate.

**Wayland.** A display system used on Linux.

**Worker (Lua).** A Lua script that runs on a thread of its own, in a Lua state of its own, and sends values to the plugin that started it (3.4).

**Workflow (GitHub Actions).** A file in `.github/workflows/` that tells GitHub which jobs to run, and when.

**Workspace.** A named arrangement of panels and OS windows that can be switched to instantly.

**X11.** The older display system used on Linux.

**XDG Base Directory specification.** The freedesktop.org rules for where Linux programs keep configuration (`~/.config`), data (`~/.local/share`) and state (`~/.local/state`).

**xdg-activation.** A Wayland protocol through which an application asks the compositor to focus one of its windows.
