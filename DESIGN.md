# OpenECS Technical Design

**Status:** design stage. Nothing is implemented yet.

**What this document is:** how OpenECS is built: modules, interfaces, data, rules and dependencies. Read [OVERVIEW.md](OVERVIEW.md) first. This document uses its words (*panel*, *panel type*, *plugin*, *service*, *preset*, *session* and so on) and does not repeat it.

**How to read it:**

- **[Decided]**: settled. **[Proposed]**: recommended, but not confirmed by the owner yet.
- A tag on a heading applies to everything under it, unless a line has its own tag.
- Open questions and pending work are in [TODO.md](TODO.md).
- Code samples show the shape of an interface, not final names.
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
18. Rules for contributors
19. Platform notes
- Glossary

---

## 1. Conventions

- [Decided] Every public C name of the core (function, type, macro) starts with `ECS_`. Every Lua name of the core lives under the global table `ecs`.
- [Proposed] The core's own settings, events and bindable functions also start with `ecs.`, for example the setting `ecs.focus`.
- [Decided] Other naming conventions, such as case style and file names, come from the owner (TODO.md). Until then, names in this document are only examples.
- [Decided] The core is written in C. Lua is the standard implementation, latest release.
- [Proposed] The core uses C23, which is the default of GCC 15. The plugin header uses only C11 and is also valid C++, so any C or C++ compiler can build plugins.
- [Decided] Linux only.
- [Decided] Out of scope: security (plugins and files are trusted) and accessibility. Look and feel is not a priority.

## 2. Program structure

### 2.1 Modules [Proposed]

| Module   | Job                                                                                                                      |
| -------- | ------------------------------------------------------------------------------------------------------------------------ |
| Platform | OS windows, raw input, clipboard, dialogs, loading shared libraries. The only module that talks to the operating system. |
| Renderer | Puts surfaces and the core's own interface on screen, behind a small interface (5.4).                                    |
| Layout   | Layout trees, workspaces, hit testing, docking. The only module that calls Clay.                                         |
| Input    | Focus, pointer routing, key dispatch, text input.                                                                        |
| Events   | Core and plugin events, the event queue, timers.                                                                         |
| Plugins  | Finding, ordering and loading plugins.                                                                                   |
| Services | Function registry, signatures, calls between C and Lua. The only module that calls libffi.                               |
| Lua      | The Lua state. Runs all Lua code in protected calls.                                                                     |
| Settings | Declarations, layers, explanations.                                                                                      |
| Session  | Reading presets and sessions, applying them, writing them.                                                               |

### 2.2 The boundary [Decided]

- No header that plugins include, and no function that plugins call, exposes a type from SDL, Lua, Clay or libffi.
- Native plugins reach SDL only through the built-in **sdl** plugin (9.8).

### 2.3 Start-up [Proposed]

1. Read the command line (13.5) and find the preset or session.
2. Start Lua. Read the preset's identity, plugins and settings as data (11.4).
3. Give SDL the tool's identity (name, icon, app id) with `SDL_SetAppMetadata`. [Decided] This happens before any OS window exists, because SDL needs the identity before it starts.
4. Start the platform layer and the renderer.
5. Find plugins, read their manifests, resolve dependencies and compute the load order (9.3).
6. Load each plugin in order and call its `init`. Plugins register what they provide.
7. Build the settings layers (12.2).
8. Restore plugin state, then build the layout and panels, from this tool's last session or from the preset (13.4).
9. Enter the main loop.

### 2.4 Shutdown [Proposed]

1. [Decided] Ask about unsaved work (4.5). The user may cancel.
2. [Decided] Save the session for the next start (13.4).
3. Destroy the panels. Call each plugin's `shutdown` in reverse load order.
4. Stop Lua and the platform layer.

## 3. Main loop, timers and threads

### 3.1 The loop

[Decided] OpenECS is event-driven: nothing runs unless something happened. The loop wakes up only when an input event arrives, a timer is due, background work finishes, or a frame is due because a visible panel draws continuously.

[Proposed] One pass of the loop:

1. Wait for one of the reasons above.
2. Handle input: hit testing, focus and key dispatch (7).
3. Run the timers that are due.
4. Deliver queued events (8).
5. Recompute the layout if it changed.
6. Draw the visible panels that need it: continuous panels, and panels that asked to be redrawn or changed size or scale.
7. Compose and present.

[Proposed] The wait uses `SDL_WaitEventTimeout` with the time until the next timer. Continuous drawing is paced by the display's refresh (vsync).

### 3.2 Timers [Decided]

- Plugins ask for timers: once or repeating, with an interval. There is no `update` call for every panel on every frame, and no sleep state.
- Timers run on the main thread, also for panels in hidden workspaces.
- [Proposed] Illustrative: `ECS_TimerStart(ctx, 0.1, true, fn, data)` returns a timer, `ECS_TimerStop(timer)` stops it. Lua: `ecs.timer.start(0.1, true, fn)`. A timer that belongs to a panel stops when the panel closes.

### 3.3 Threads

- [Proposed] OS windows, input, layout, event delivery, timers, every call into Lua and every callback that a plugin registers run on the main thread. SDL expects window and event calls there, and a Lua state must not be used by two threads at once.
- [Decided] Background work posts its result to the main thread, which delivers it.
- [Proposed] `ECS_RunInBackground(ctx, work, done, data)`: `work` runs on a worker thread from a small pool, then `done` runs on the main thread. Lua code never runs on worker threads; Lua plugins use services that do their work in the background.
- [Proposed] The thread-safe functions are: running a function on the main thread (`ECS_RunOnMainThread`, built on `SDL_RunOnMainThread`), `ECS_RunInBackground`, and logging. Every other function is for the main thread only. Each function documents its thread rule.
- [Proposed] Native plugins may create their own threads, under the same rule.

## 4. Panel types and panels

### 4.1 Panel type descriptor [Proposed]

```c
typedef struct ECS_PanelTypeDesc {
    uint32_t        struct_size;   /* sizeof this struct (9.5)                  */
    const char     *name;          /* "canvas.view": plugin name + local name   */
    const char     *title;         /* default title for tabs and menus          */
    uint32_t        state_version; /* version of the saved state                */
    ECS_SurfaceKind surface;       /* ECS_SURFACE_PIXELS or ECS_SURFACE_GPU     */
    bool            continuous;    /* draw every frame while visible            */
    float           min_width;     /* in layout units, 0 for none               */
    float           min_height;

    /* required */
    void *(*create)(ECS_Panel *panel, const ECS_Value *saved_state, uint32_t version);
    void  (*destroy)(void *state);

    /* optional, NULL if unused */
    void  (*draw)(void *state, ECS_Surface *surface, double seconds);
    void  (*event)(void *state, const ECS_Event *event);
    ECS_Value (*save_state)(void *state);
    bool  (*save)(void *state);    /* saves unsaved work; false on failure     */
} ECS_PanelTypeDesc;
```

- A Lua panel type is a table with the same field names.
- `draw` receives the time in seconds since the panel was last drawn, for animation.
- `event` returns nothing, because events never ask for permission (8.1).
- Registration: `ECS_RegisterPanelType(ctx, &desc)`, and in Lua `ecs.panel.register_type(desc)`.

### 4.2 Panels [Proposed]

- A panel has a stable id, its type, the state its type created, a title, a size, a scale, a visibility and an unsaved-work flag.
- Lifecycle: `create` (with saved state when restoring), then `draw` and `event` calls, then `destroy`.
- [Decided] Many panels can share one type, each with its own state.
- Functions a panel calls about itself (illustrative): `ECS_PanelRedraw`, `ECS_PanelSetTitle`, `ECS_PanelSetUnsaved`, `ECS_PanelSetCursor`, `ECS_PanelLockPointer`, `ECS_PanelSetTextInput`, `ECS_PanelAcceptDrops`, `ECS_PopupOpen`.

### 4.3 When a panel is drawn [Decided]

- A continuous panel is drawn every frame while it is visible.
- Any other panel is drawn when it asks (`ECS_PanelRedraw`) or when its size or scale changes.
- Panels in hidden workspaces are not drawn. A hidden panel runs only when its timers or events call it.

### 4.4 Placeholders [Decided]

- A panel whose type is missing becomes a placeholder that keeps its saved state unchanged, so saving writes it back.
- [Proposed] A panel whose callback raised an error becomes a faulted placeholder (14.2).

### 4.5 Unsaved work [Decided]

- Plugins cannot refuse an operation. Events are notifications only.
- A panel sets its unsaved-work flag. Before such a panel closes, whether the user closes it, the user quits or another session is being loaded, the core asks: Save, Discard or Cancel. Save calls the type's `save`; if that fails, the close is cancelled.
- [Proposed] The question is a message dialog (`SDL_ShowMessageBox`). When several panels have unsaved work, one dialog lists them all.

### 4.6 Popups, pointer and text input [Decided]

- **Popups.** A panel opens a popup anchored to a rectangle in its own area. A popup has its own surface, may extend past the panel and the OS window, and closes on Escape, on a click outside it, or when its panel closes.
  - [Proposed] Popups are SDL popup windows (`SDL_CreatePopupWindow`). SDL keeps them inside the display and hides them with their parent. Menus take keyboard focus; tooltips do not.
- **Pointer.** A panel sets the pointer's shape while the pointer is over it: a system shape or an image. A focused panel can lock the pointer (relative mode, for 3D cameras). The lock ends when the panel loses focus. [Proposed] Pressing the core prefix also ends it.
- **Text input.** A panel says whether it accepts text and where its text cursor is. The core turns text input on for the focused panel and tells the input method where to show its window (`SDL_StartTextInput`, `SDL_SetTextInputArea`).

## 5. Surfaces and rendering

### 5.1 Surfaces [Decided]

- Every panel has a surface. The core offers no drawing commands.
- There are two kinds: **pixels** (memory written by the processor) and **GPU** (a texture on the graphics card).
- Sizes are in physical pixels. Each panel has a **scale** (physical pixels per layout unit) and gets an event when the scale changes.
- [Proposed] Pointer positions given to a panel are in its surface's pixels, so they match what it draws.
- [Proposed] A panel must not keep its surface after `draw` returns; every call receives the current one. So the core can recreate surfaces, for example on resize, without telling the panel.

```c
typedef struct ECS_Surface {
    ECS_SurfaceKind kind;
    int    width, height;        /* physical pixels                          */
    float  scale;                /* physical pixels per layout unit          */

    void  *pixels;               /* pixels kind: the first row               */
    int    pitch;                /* pixels kind: bytes per row               */

    ECS_GpuTexture *texture;     /* GPU kind: opaque; the sdl plugin turns it */
                                 /* into an SDL_GPUTexture                    */
} ECS_Surface;
```

### 5.2 Pixel format [Proposed]

- 32 bits per pixel, `ARGB8888` in native byte order, with premultiplied alpha. This is the native format of Cairo (`CAIRO_FORMAT_ARGB32`), pixman and Blend2D, so plugins that use them draw straight into the surface.
- Panels are opaque, so alpha is ignored for panels. It matters only for popups.
- A panel may report which rectangles changed. If it reports none, the whole surface is uploaded.

### 5.3 The renderer [Decided]

- One SDL GPU device serves every OS window. Each OS window has a 2D renderer on that device, made with `SDL_CreateGPURenderer(device, window)` (SDL 3.4.0 and later).
- The core draws its own interface with that 2D renderer: Clay for layout, SDL3_ttf for text.
- A GPU surface is a texture on the shared device. Any OS window's renderer can show it by wrapping it (`SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER`), so a GPU panel moves between OS windows without recreating anything.
- A pixels surface is uploaded to a texture when it changed.
- Evidence: SDL's own test `test/testgpu_spinning_cube.c` claims several windows with one GPU device and creates a GPU renderer on a shared device. A prototype must still confirm the whole path (TODO.md).

### 5.4 Replaceable [Decided]

- The renderer can be replaced without breaking plugins, because plugins see only surfaces. Plugins with pixels surfaces never notice. GPU plugins depend on the **sdl** plugin's contract instead (9.8).
- [Proposed] The renderer's interface: create, resize and destroy a surface; upload pixels; draw the core's interface from Clay's output; present a frame.

### 5.5 How plugins draw [Proposed]

- GPU panels get the GPU device and their texture through the **sdl** plugin (9.8) and draw with SDL's GPU API.
- The **ui** plugin draws with SDL's 2D renderer and SDL3_ttf. It creates an offscreen renderer on the shared device (`SDL_CreateGPURenderer(device, NULL)`, as SDL's test does) and draws into the panel's texture. So the core and the ui plugin use the same stack.
- The ui plugin draws only into GPU surfaces. Plugins that compute their own pixels use pixels surfaces.

## 6. Layout

### 6.1 Data [Proposed]

- Each OS window is a **root**: the OS window, its layout tree, and its maximized group, if any.
- Node kinds:

| Kind  | Holds                                                                                                         |
| ----- | ------------------------------------------------------------------------------------------------------------- |
| Split | a direction; ordered children; for each child, a fixed size in layout units or a share of the remaining space |
| Group | ordered panels; the panel shown; whether it is locked                                                         |
| Panel | a panel                                                                                                       |

- [Decided] Every panel is in a group, even when it is alone.
- [Decided] Split children have a fixed size or a share. Groups can be locked.
- [Decided] A **workspace** is a name and a list of roots. A pop-out root belongs to the workspace in which it was created.

### 6.2 Tidying [Decided]

After every operation:

- A split with one child is replaced by that child.
- A split inside a split of the same direction is merged into it.
- An empty group is removed.
- Node and panel ids stay stable and unique within a session.

### 6.3 Sizes [Decided]

- Fixed-size children keep their size. Shares divide the space that is left, so resizing keeps proportions.
- A split never makes a child smaller than the minimum size of its panels.
- [Proposed] If an OS window is too small for all minimums, the last children are clipped. Dragging a divider next to a fixed-size child changes that child's fixed size.

### 6.4 Tab rows and grips

- [Decided] A group shows a tab row when it holds two or more panels. A group with one panel shows none.
- [Decided] The user cannot move or close the panels of a locked group.
- [Decided] A panel without a tab row shows a grip at its top centre while the pointer is within a few pixels of its top edge. Dragging the grip moves the panel; clicking it opens the panel's menu. While the grip is shown, pointer events over it go to the core.
- [Proposed] Locked groups show no grip and accept no dropped panels.

### 6.5 Operations [Decided]

- Every layout change is a plain function with a Lua counterpart. Pointer gestures only compute the arguments.
- [Proposed] Operations: open a panel, split, move to a target and zone, group, close, pop out, swap, resize a divider, focus in a direction, maximize, switch workspace, cycle tabs, reopen the last closed panel. Illustrative: `ECS_LayoutMove(panel_id, target_id, ECS_ZONE_LEFT)` and `ecs.layout.move(panel_id, target_id, "left")`.

### 6.6 Placement of new panels [Decided]

- A panel opened by code goes to a predictable default place. The caller can pass another place instead: a group, a side of a panel (split), or a new OS window.
- [Proposed] The default: the group of the most recently focused panel of the same type; if there is none, the focused group.

### 6.7 Drop zones [Proposed]

While a panel is dragged, the zones are checked in this order:

1. Outside every OS window: pop out.
2. Within 16 layout units of an OS window's edge: dock along that whole edge.
3. Over a tab row: insert between tabs, at the nearest gap between tab midpoints.
4. In the outer quarter of a panel, at most 80 layout units deep: split toward that side.
5. Anywhere else over a panel: group with it.

On release, the matching operation is called. In small panels, the edge bands shrink so that the centre stays at least a third of the panel.

### 6.8 Clay

- [Decided] Clay computes the rectangles. It is a pinned git submodule.
- [Proposed] Only the Layout module calls Clay, because Clay's API changes between versions. There is one Clay context per OS window (`Clay_SetCurrentContext`).

### 6.9 Maximize, pop-out and workspaces [Decided]

- Maximize marks one group as filling its root. The other panels stay alive but hidden. The mark is saved in the session.
- Pop-out creates an OS window with a new root that holds one group with the panel.
- Leaving a workspace hides its pop-out windows; returning shows them.
- Switching workspaces is instant: nothing is destroyed or rebuilt.

## 7. Input, focus and keys

### 7.1 Focus [Decided]

- Exactly one panel has keyboard focus.
- The setting `ecs.focus` is `click` or `hover`. [Proposed] The default is `click`.
- Keys move focus to the neighbouring panel.
- [Proposed] The neighbour is chosen by geometry: the nearest panel in that direction that overlaps on the other axis. This does not need the tree.
- [Proposed] In hover mode, only real pointer movement changes focus. A layout change under a still pointer does not, and neither does the pointer over a divider or a tab row.
- [Proposed] On Wayland, the compositor decides whether an application may raise one of its own windows (SDL uses the xdg-activation protocol). Moving focus to another OS window is best effort there.

### 7.2 Pointer routing [Decided]

- Pointer events go to the panel under the pointer, whatever has focus.
- While a button is held, pointer events keep going to the panel where the press started, even outside its OS window. SDL captures the mouse while a button is held by default (`SDL_HINT_MOUSE_AUTO_CAPTURE`).

### 7.3 Key combinations [Proposed]

- A key combination is modifiers plus one key, written as text: `Ctrl+Shift+P`. The modifiers are Ctrl, Shift, Alt and Super. The text is case-insensitive, and key names are SDL's.
- Bindings match SDL key codes, which follow the user's keyboard layout. SDL's default `latin_letters` option makes the letter keys of non-Latin layouts report English letters, so shortcuts work on every layout.
- [Decided] AltGr (`SDL_KMOD_MODE`) is never part of a binding. A key pressed with AltGr held is text.

### 7.4 Dispatch [Decided]

When a key is pressed:

1. The core prefix (7.5) is checked first, at the moment of the key press. So changing the prefix while OpenECS runs can never hand it to plugins. A plugin binding that uses the prefix's combination is not triggered and is reported. After the prefix, the next key press goes to the core.
2. Among the bindings whose scope is active (the focused panel, its panel type, the current workspace, the whole tool), the binding whose key was set in the highest settings layer wins (12.2). On a tie, the most specific scope wins.
3. If nothing matches, the key goes to the focused panel as a raw key event.

A key press that triggers a binding produces no text input event.

### 7.5 The core prefix

- [Decided] The core reserves one key combination, the **core prefix**, kept in the setting `ecs.prefix`. It never reserves a whole modifier.
- [Decided] After the prefix, the next key runs a core action from the setting `ecs.prefix_keys`. While the core waits for that key, it shows the available keys; Escape cancels. Presets and the user can add entries that run service functions.
- [Decided] The prefix and the key after it are the only key sequence the core handles. Everything else is keys pressed together; longer sequences are up to panels.
- [Decided] The default prefix is `Alt+W`. It types no text, and GNOME, KDE, Omarchy and VS Code do not use it by default. Other two-key choices are taken:
  - `Ctrl+Space` is grabbed by the fcitx5 input method before applications see it. Omarchy hit this conflict with its own tmux prefix.
  - `Alt+Space` is reserved by GNOME (window menu) and KDE (launcher).
  - `Ctrl+W`, `Ctrl+G` and `Ctrl+B` are common editor shortcuts: close, go to line, toggle sidebar.
- [Decided] The default `ecs.prefix_keys`:

  | Key          | Action                 |
  | ------------ | ---------------------- |
  | Arrows       | Move focus             |
  | Shift+Arrows | Move the focused panel |
  | 1 to 9       | Switch workspace       |
  | Tab          | Show the next tab      |
  | M            | Maximize or restore    |
  | P            | Pop out                |
  | X            | Close the panel        |
  | Escape       | Cancel                 |

### 7.6 Binding keys [Decided]

- Keybindings are settings of type `key` and follow the settings layers (12).
- A plugin binds keys only for its own panel types. Those bindings work only while one of its panels has focus. Plugins have no function for workspace or global bindings.
- Presets and the user's settings may bind workspace and global keys to registered service functions, by name.
- [Proposed] `ECS_BindKey(ctx, panel_type, setting_name, fn)`: the setting holds the key; the plugin supplies the function.
- [Proposed] A function bound by name takes no arguments, or one argument: the focused panel.
- [Proposed] The core registers its own bindable actions as functions under `ecs`, for example `ecs.focus_left` and `ecs.maximize`. So settings name them like any plugin function.

## 8. Events

### 8.1 Kinds [Proposed]

- Core events: panel opened, closed, resized, scale changed, shown, hidden, focused, unfocused, moved, popped out, grouped, maximized; workspace switched. Input events (key, pointer, text, drag and drop) go to the panel concerned.
- [Decided] Plugin events: a plugin declares named events, such as `canvas.selection_changed`, and emits them with a value (10.3).
- [Decided] Events are notifications. Handlers return nothing and cannot cancel anything.

### 8.2 Delivery

- [Decided] Events are queued and delivered on the main thread after the current callback returns, never inside another event handler.
- [Proposed] Changes take effect immediately; only the notification waits. A closed panel leaves the layout at once, but its `destroy` runs after the current delivery finishes, so no handler meets a destroyed panel.

### 8.3 Who receives what [Proposed]

- Input events go only to the panel they concern.
- Any plugin may subscribe to the layout and lifecycle events of any panel, to read them.
- A plugin may subscribe to another plugin's events only if its manifest depends on that plugin.
- Illustrative: `ECS_EventDeclare(ctx, "canvas.selection_changed", description)`, `ECS_EventEmit(ctx, name, value)`, `ECS_EventSubscribe(ctx, name, fn)`. In Lua: `ecs.event.declare`, `emit` and `subscribe`.

## 9. Plugins

### 9.1 On disk [Proposed]

```
plugins/
  canvas/
    manifest.lua   -- description and dependencies
    canvas.so      -- native code (optional)
    init.lua       -- Lua code (optional)
```

- Paths in a manifest are relative to the manifest.
- A plugin may have both kinds of code. The native `init` runs first, then `init.lua`.

### 9.2 Manifest [Proposed]

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

- Versions follow semantic versioning. A dependency's version means "this version, or a later one with the same major number", as in Cargo: `"1.2"` accepts 1.2.0 up to, but not including, 2.0.0.
- [Decided] Manifests are read as data, without running plugin code (11.4).
- [Decided] Presets follow the same rules as manifests: they name the plugins they depend on.

### 9.3 Search path and load order

- [Decided] Plugins are found by scanning plugin directories. A preset can name an extra directory.
- [Proposed] Search order: the preset's directory, the user's plugin directory, then the first-party plugins shipped with OpenECS (16). The first plugin found with a given name wins, so a user can replace a first-party plugin.
- [Decided] The core computes the load order from the dependencies (topological order). Missing dependencies, version mismatches and cycles are reported, and the plugins affected are skipped (14.2). Plugin authors never manage the order.
- [Decided] The active plugins are fixed at start-up: the plugins the preset names and their dependencies. [Proposed] Plus the plugins the user's settings name (12.4).

### 9.4 Native plugin interface

- [Decided] The plugin API version is checked before any other plugin code runs.
- [Proposed] Plugins do not link against the core. `init` receives a **context**: a versioned table of function pointers that belongs to that plugin, and through which the core knows who is calling.
- [Proposed] A native plugin exports exactly one function, which describes it:

```c
typedef struct ECS_PluginInfo {
    uint32_t struct_size;
    uint32_t api_version;
    bool (*init)(ECS_Context *ctx);       /* registers everything; false on failure */
    void (*shutdown)(ECS_Context *ctx);
} ECS_PluginInfo;

const ECS_PluginInfo *ECS_PluginMain(void);   /* the only exported name */
```

- [Proposed] Plugins are loaded with `SDL_LoadObject` and `SDL_LoadFunction`.

### 9.5 ABI rules [Proposed]

- A structure that crosses the boundary starts with its own size (`struct_size`), so the core knows which fields exist.
- Structures only grow at the end. Fields are never removed, reordered or changed in meaning.
- Structures are passed by pointer, never by value.
- Enumeration values are never renumbered.
- Plain C only: no C++ exceptions and no `longjmp` across the boundary.

### 9.6 Lua plugins [Decided]

- All Lua plugins share one Lua state. Each plugin's code runs in its own environment (its own table of globals), which contains the `ecs` table.
- [Proposed] The core knows which plugin made an `ecs` call from the environment it came from. That is how the rules of least authority apply to Lua plugins.

### 9.7 Lifecycle

- [Decided] Plugins are not activated or deactivated while OpenECS runs.
- [Proposed] Native plugins are never unloaded before exit, because their function pointers, threads and static data may still be in use.
- [Proposed] The core records everything each plugin registers. If `init` fails, the plugin's registrations are removed and it is marked failed.

### 9.8 Built-in sdl plugin [Decided]

- The executable contains a binding plugin named `sdl`, registered like any plugin. Its service gives native plugins SDL objects: the GPU device, a panel's GPU texture, and the SDL version.
- [Proposed] SDL3 and SDL3_ttf are shared libraries shipped next to the executable (17.2). So the core, the sdl plugin, and any plugin that links an SDL library such as SDL3_mixer or SDL3_net all use one copy of SDL.

### 9.9 Names

- [Decided] The prefix `ecs` belongs to the core.
- [Proposed] Everything a plugin registers is named `<plugin name>.<local name>`: functions (`canvas.new`), panel types (`canvas.view`), events, settings (`canvas.grid`) and keybindings. The core rejects other names, so names cannot collide.

## 10. Services

### 10.1 Registration [Decided]

- Functions are registered with a declared signature, checked at registration. An invalid signature is rejected right then, not when the function is called.
- A plain C function can be registered without a hand-written wrapper.
- [Proposed] Each function also has a one-line description, so menus and key-binding editors can show it.

Illustrative:

```c
ECS_RegisterFunction(ctx, "audio.play", audio_play, "int(string, float)", "Play a sound file");
```

```lua
ecs.service.register("audio", {
  play = { sig = "int(string, float)", doc = "Play a sound file",
           fn = function(path, volume) ... end },
})
```

### 10.2 Signature types [Proposed]

| Type              | Meaning                                            |
| ----------------- | -------------------------------------------------- |
| `void`            | no value (return only)                             |
| `bool`            | true or false                                      |
| `int`, `int64`    | whole numbers                                      |
| `float`, `double` | decimal numbers                                    |
| `string`          | text ending in a zero byte                         |
| `buffer`          | a pointer and a length                             |
| `handle<name>`    | a typed handle (10.6)                              |
| `value`           | a generic value (10.3)                             |
| `fn<signature>`   | a function to call back                            |
| `out <type>`      | an output parameter; in Lua, an extra return value |

- [Decided] Callbacks and output parameters are supported.
- Structures are passed as handles or buffers, never by value.
- There is no fixed limit on the number of parameters.

### 10.3 Values [Proposed]

A generic value (`ECS_Value` in C) is nil, a boolean, an integer, a number, a string, a buffer, a handle, or a table (a list or named fields). Saved state, plugin events and generic calls use values.

### 10.4 Calls [Proposed]

- **Lua calls C:** the core makes one Lua function for each registered C function. It converts the arguments by the signature, calls the C function through libffi, and converts the results back.
- **C calls C:** the caller gets the raw function pointer and calls it at full speed.
- [Decided] **C calls Lua:** the caller also gets a typed C function pointer. libffi creates it as a closure: a small piece of generated code that converts the arguments, calls the Lua function in a protected call, and converts the result. So a C caller never needs to know which language the provider uses.
- **Lua calls Lua:** a plain Lua call, because all plugins share one Lua state.
- [Decided] Callbacks (`fn<...>` parameters) work the same way in both directions.

### 10.5 Lookup

- [Decided] A plugin asks for a function by name and states the signature it expects. The core compares it with the registered signature and refuses a mismatch, so a version mismatch shows up at lookup instead of crashing a call.
- [Proposed] A plugin may look up only functions of plugins named in its manifest's dependencies. If a provider fails, its users are told.
- Illustrative: `ECS_GetFunction(ctx, "audio.play", "int(string, float)")`.

### 10.6 Handles [Proposed]

- A handle stands for an object owned by its provider: a pointer plus a type name and a destructor, registered with `ECS_RegisterHandleType`.
- In Lua, a handle is a userdata whose metatable names its type. A handle of the wrong type is rejected with a clear error. When Lua no longer uses a handle, its garbage collector calls the destructor.
- When a provider goes away, its handles become invalid and their users are told.

### 10.7 Buffers [Proposed]

- A buffer argument is valid only during the call. A function that needs it longer copies it.
- A returned buffer's lifetime is documented by its function. Lua callers always receive a copy.

### 10.8 Errors in services

- [Decided] All service functions report errors in one consistent way.
- [Proposed] In C, the same way as the core (14.3). In Lua, expected failures, such as a missing file, return `nil, message`; misuse, such as a wrong argument type, raises an error. This is the convention of Lua's own library.

### 10.9 libffi

- [Decided] libffi calls functions by declared signature. It is a git submodule.
- [Proposed] Only the Services module calls libffi. Each function's call description is prepared once, at registration.
- The core cannot check that a C function really matches its declared signature. A wrong declaration is a bug in the plugin and may crash the program.
- libffi supports Linux on x86-64 and AArch64, closures included.

## 11. Lua

### 11.1 Version [Decided]

The standard Lua implementation, latest release.

### 11.2 One state [Decided]

One Lua state, with one environment per plugin (9.6).

### 11.3 Protected calls [Proposed]

Every call from the core into Lua is a protected call. Lua reports errors by jumping out of the current function, so an unprotected call could unwind through core code. A caught error becomes an error report (14), never a crash.

### 11.4 Data files

- [Decided] Manifests, presets, sessions and settings files are read as data. They run without `ecs`, so they cannot call the core or plugins while they are read.
- [Proposed] They are loaded as text only (`load(text, name, "t", env)`). The environment holds Lua's basic functions and the `string`, `table`, `math` and `utf8` libraries; there is no `io`, `os` or `require`. A preset can still use loops and conditions.

### 11.5 The `ecs` table [Proposed]

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

### 11.6 Parity

- [Decided] Everything the core can do is available from Lua.
- [Proposed] A list of every public C function with its Lua counterpart is kept and checked by the build or the tests.

### 11.7 Keeping Lua values [Proposed]

Lua functions and values that C code keeps are stored in Lua's registry and referenced by a number. They are released when their owner goes away.

## 12. Settings

### 12.1 Declaring [Proposed]

- The core and plugins declare settings with a name, a type, a default and a description. Owners are told when their settings change.
- Types: `bool`, `integer`, `number`, `string`, `choice` (one of a list), `key` (a key combination), `list` and `table`.

### 12.2 Layers [Decided]

From lowest to highest; a higher layer overrides a lower one:

1. Core defaults.
2. Plugin defaults.
3. The preset.
4. The settings window's file, written by OpenECS.
5. The user's hand-edited file.

- The core never rewrites the hand-edited file.
- Unknown keys, for example those of plugins that are not loaded, are kept.

### 12.3 Interface [Proposed]

- `get(name)`, `set(name, value)` (writes layer 4), `list()` (every declared setting) and `explain(name)`: the value in effect, the layer it came from, and what each layer says.
- The settings window plugin uses `list` and `explain`. It shows a setting that layer 5 sets as locked, with the name of the file.

### 12.4 User files [Proposed]

Each user file has a part for every tool and a part per tool, keyed by the tool's app id. Within one file, the tool's part wins. A user file can also name extra plugins to load in every tool.

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

### 13.1 Format [Decided]

Presets and sessions share one format: a Lua file that returns a table. A preset is hand-written and may use logic. A session is written by OpenECS and is a snapshot, not a link back to its preset. Lua is the format, so no separate parser is written.

### 13.2 Fields [Proposed]

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
- Sessions also store panel ids, the focused panel, the maximized group, each saved state's version, and `plugin_state`, the state of each plugin.
- The app id should match the name of the tool's `.desktop` file, so that the desktop connects the windows to the right application.

### 13.3 Applying a session

[Proposed] Steps:

1. Read the file as data (11.4) and check its shape. An error names the path of the problem, for example `workspaces[1].windows[2]`.
2. Make sure the plugins it needs are active.
3. Restore each plugin's state, with its version.
4. Build the layout hidden, and create each panel with its saved state.
5. Show everything at once.

[Decided] Rules:

- If a panel's type is missing, a placeholder keeps its saved state unchanged.
- If restoring a panel fails, it becomes a placeholder; everything else loads.
- Each panel type and plugin owns its state format and version, and is told which version it reads. The core converts only layout data.

### 13.4 Saving [Decided]

- On a clean exit, after the unsaved-work question, OpenECS saves the session. At the next start of the same tool, that session is restored instead of the preset.
- The user can save the session to a file at any time (an action, also callable from Lua) and load it later.
- There is no automatic saving while OpenECS runs.
- A file is never left half-written.
- [Proposed] The command-line option `--fresh` starts from the preset instead of the last session.
- [Proposed] Only data is written: numbers, strings, booleans and tables. Functions and reference cycles are an error.
- [Proposed] The writer is a short Lua function embedded in the executable. `string.format("%q", x)` writes strings safely and numbers exactly (decimal fractions in hexadecimal form), so reading gives back the same values.
- [Proposed] A file is written to a temporary file, then renamed over the old one.

### 13.5 Command line [Proposed]

```
openecs [--preset NAME|FILE] [--session FILE] [--fresh] [FILE...]
```

- Without a preset or session, the launcher plugin starts.
- Files are passed to the function that the preset names in `open`.
- A tool's `.desktop` file runs, for example, `openecs --preset paint %F`.

## 14. Errors and logging

### 14.1 Principles

- [Decided] A native plugin that crashes takes the program down. This cannot be prevented and is accepted.
- [Decided] Everything else stays local: a Lua error or a failing panel does not stop the core.
- [Proposed] An error report holds the plugin, the kind of error, a message and, for Lua, a stack trace. It goes to the log. The user sees it where it applies: a faulted panel shows it; plugin loading problems appear in the launcher and the log. A message dialog is used only when start-up fails.

### 14.2 Policies [Proposed]

| Situation                                                                             | What happens                                                                                                                                       |
| ------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| Manifest unreadable or invalid                                                        | The plugin is skipped and reported, and so are its dependents.                                                                                     |
| Missing dependency or version mismatch                                                | The plugin and its dependents are skipped and reported.                                                                                            |
| Dependency cycle                                                                      | The plugins in the cycle are skipped and reported.                                                                                                 |
| Plugin API version mismatch                                                           | The plugin is refused before any of its code runs.                                                                                                 |
| `init` fails                                                                          | The plugin is marked failed and its registrations are removed.                                                                                     |
| Invalid registration (bad signature, bad or duplicate name, the core prefix as a key) | That registration is rejected and the error is returned to the plugin, which continues.                                                            |
| A panel callback raises an error                                                      | The panel becomes a faulted placeholder that shows the error. Its menu has a "Restart" entry, which recreates the panel from its last saved state. |
| A plugin callback (timer, event handler) raises an error                              | The error is reported. Repeats of the same error are counted, not reported again.                                                                  |
| Problems while restoring a session                                                    | See 13.3.                                                                                                                                          |

### 14.3 Error convention [Proposed]

- C: functions return `bool`, or a pointer that is `NULL` on failure. `ECS_GetError()` returns the calling thread's last error message, like `SDL_GetError`.
- Lua: as in 10.8.

### 14.4 Logging [Proposed]

- Levels: error, warning, info and debug.
- Each line holds the time, the level, the plugin and the message. The core adds the plugin's name from the context.
- Lines go to standard error and to the log file (16).

## 15. Memory and ownership [Proposed]

- Whoever allocates memory frees it. The core never frees plugin memory, and never returns memory that a plugin must free.
- Strings passed into a function are valid during the call. Keep a copy to use them later.
- Memory returned by the core stays valid until a documented point, for example "until the next call".
- Objects shared between plugins are handles with destructors (10.6).
- No shared allocator is needed, because ownership never crosses the boundary.

## 16. Files and directories [Proposed]

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

- The specification names "layout, open files" as examples of what belongs in `$XDG_STATE_HOME`, which is exactly a tool's last session.
- SDL has no function for these directories (`SDL_GetPrefPath` returns a data directory), so the core reads the XDG variables itself.

## 17. Build and dependencies

### 17.1 Dependencies

| Library                     | Status                                                             |
| --------------------------- | ------------------------------------------------------------------ |
| SDL3                        | [Decided] core                                                     |
| SDL3_ttf                    | [Decided] core: text in the core's interface                       |
| Clay                        | [Decided] core: layout; a git submodule                            |
| Lua                         | [Decided] core                                                     |
| libffi                      | [Decided] core: calls by signature; a git submodule                |
| SDL3_image                  | [Decided] not used: SDL 3.4 loads PNG files itself (`SDL_LoadPNG`) |
| SDL3_mixer, SDL3_net, cgltf | [Decided] not core; plugins may use them                           |

### 17.2 Linking [Proposed]

- SDL3 and SDL3_ttf are shared libraries shipped next to the executable and found through an `$ORIGIN` run path. SDL keeps global state, so two copies in one process would conflict; this way every part of the process shares one copy.
- Lua, Clay and libffi are linked statically into the executable.
- The executable exports no symbols. Plugins reach the core only through their context.
- Native plugins are shared libraries, built against the plugin header only.

### 17.3 Compiler [Proposed]

- C23 for the core. The plugin header is C11 and valid C++ (1).
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Debug builds add `-fsanitize=address,undefined`.

### 17.4 Dependency versions [Proposed]

Every dependency is a git submodule pinned to a release tag, not to a development commit.

The build system itself is an open question (TODO.md). The repository currently builds with shuild.

## 18. Rules for contributors [Proposed]

### 18.1 Core

1. Never expose an SDL, Lua, Clay or libffi type in a header that plugins include.
2. Every public function has a Lua counterpart, or a written reason why not.
3. Every public function documents its thread rule and who owns what it takes and returns.
4. Every call into Lua is a protected call.
5. Only Layout calls Clay; only Services calls libffi; only Platform, the Renderer and the sdl plugin call SDL.
6. Core code never checks a plugin's name to decide what to do. First-party plugins get no special treatment.
7. If the core's own machinery needs something, it is core; otherwise it is a plugin.

### 18.2 Native plugins

1. Register things only inside `init`.
2. Call the core only from the threads its documentation allows.
3. Do not free the core's memory, and do not keep core pointers longer than documented.
4. No C++ exceptions and no `longjmp` across the boundary.
5. Declare signatures accurately.
6. Do not keep a surface after `draw` returns.

## 19. Platform notes

- [Decided] Linux only. SDL3 supports both X11 and Wayland.
- [Proposed] On Wayland, the compositor decides where a new OS window appears. Dragging a panel out of its window still works, because the core keeps receiving pointer events while the button is held (7.2); only the position of the new window cannot be chosen.
- [Proposed] Popups are positioned relative to their parent window, which Wayland supports.
- [Proposed] Moving keyboard focus to another OS window is best effort on Wayland (7.1).

---

## Glossary

**ABI (application binary interface).** The rules for how compiled code fits together: where each field of a structure sits in memory, how large each type is, how functions receive arguments. Code that disagrees about the ABI breaks, even though no compiler complained.

**API (application programming interface).** The functions and types that one piece of code offers to another.

**Buffer.** A pointer to memory together with its length.

**Callback.** A function passed to other code so that it can be called later.

**Closure (libffi).** A small piece of code that libffi generates at run time. It looks like an ordinary C function, but forwards each call to a handler, which here calls a Lua function.

**Compositor.** On Wayland, the program that draws all windows on screen and decides where they go and which one has focus.

**Context.** A table of function pointers that the core gives to each native plugin. It is how a plugin calls the core without linking against it.

**Destructor.** A function that releases an object when it is no longer needed.

**Environment (Lua).** The table of global names that a piece of Lua code sees.

**Frame.** One pass of drawing and presenting.

**Function pointer.** A value that holds the address of a function, so the function can be called through it.

**GPU device.** SDL's object for the graphics card. Textures and other GPU resources belong to a device.

**Handle.** A token that stands for an object owned by someone else, so it can be passed around without exposing its inside.

**Hit testing.** Finding which element is under a point, such as the pointer.

**libffi.** A library that calls a C function when only its description (its signature) is known at run time, and that can create closures.

**Main thread.** The thread on which the program starts. OS windows, input and all Lua code run there.

**Metatable (Lua).** A table that defines how a Lua value behaves. For handles, it carries the type name.

**Offscreen renderer.** A 2D renderer that draws into a texture instead of a window.

**Premultiplied alpha.** A way of storing transparent pixels in which the colour is already multiplied by the opacity. Blending is faster and has no edge artifacts.

**Protected call (Lua).** A way to call Lua code so that an error is caught and returned, instead of jumping out through the caller.

**Registry (Lua).** A table that Lua keeps for C code to store values it needs to keep alive.

**Root.** The top of a layout tree. There is one per OS window.

**Run path.** A directory written into a program where it looks for shared libraries. `$ORIGIN` means the program's own directory.

**Sanitizer.** A compiler option that adds checks for memory errors and undefined behaviour while the program runs.

**Semantic versioning.** Version numbers of the form MAJOR.MINOR.PATCH, where a new major number means an incompatible change.

**Shared library.** A compiled code file that a program can load while it runs (`.so` on Linux).

**Signature.** A description of a function's parameter and return types.

**Struct (structure).** A group of named fields stored together in memory.

**Submodule (git).** Another repository included at a fixed version inside a repository.

**Symbol.** A name in a compiled file that other code can look up, such as an exported function.

**Texture.** An image stored on the graphics card.

**Topological order.** An order in which every item comes after the items it depends on.

**Userdata (Lua).** A block of memory managed by Lua's garbage collector that holds data from C. Handles are userdata.

**Vsync.** Waiting for the display's refresh before showing a new frame, so drawing matches the screen's rate.

**Wayland, X11.** The two display systems used on Linux. Wayland is the newer one.

**XDG Base Directory specification.** The freedesktop.org rules for where Linux programs keep configuration (`~/.config`), data (`~/.local/share`) and state (`~/.local/state`).

**xdg-activation.** A Wayland protocol through which an application asks the compositor to focus one of its windows.
