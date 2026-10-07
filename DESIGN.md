# OpenECS: Technical Design

**Status:** draft. The project is under development.

**Purpose of this document:** to describe how OpenECS is meant to be built in a real codebase: modules, programming interfaces, data structures, rules, error handling and build details. It is written for developers and AI agents who will implement or review the code.

**Relationship to the overview:** `OpenECS-design-overview.md` explains in plain language what OpenECS is and what is in scope. This document assumes that overview and does not repeat it. Words such as *behaviour*, *plugin*, *service*, *manifest*, *register*, *preset*, *session* and *workspace* are defined there. Technical terms are explained in Appendix A of this document.

**How to read it:** every statement carries one of three status tags.

- **[Decided]**: a decision that has been made.
- **[Proposed]**: a suggested design that has been discussed or follows from decisions, but is not confirmed. It may change.
- **[Open]**: not decided. All open points are collected in section 19.

Code samples are **illustrative**. They show the shape of an interface, not final names or signatures.

---

## Contents

1. Conventions
2. Program structure
3. Main loop and threading
4. Windows and behaviours
5. Surfaces and drawing
6. Layout
7. Input, focus and keybindings
8. Events and actions
9. Plugin system
10. Services and function registration
11. Lua integration
12. Settings
13. Presets and sessions
14. Error handling
15. Memory and ownership
16. Build and dependencies
17. Rules for contributors
18. Platform notes
19. Open questions and TODO
- Appendix A: Technical terms

---

## 1. Conventions

### 1.1 Naming

- **[Decided]** The core uses the prefix `ECS_` for C names and `ecs.` for Lua names. Every public C function, type and macro of the core starts with `ECS_`. Every Lua function of the core lives under the global table `ecs`.
- **[Open]** Other naming conventions (case style, file names, how plugins name their own things) will be provided by the project owner and added here.

### 1.2 Languages and versions

- **[Decided]** The core is written in C.
- **[Decided]** Lua: the standard (reference) Lua implementation, latest release. LuaJIT may be reconsidered.
- **[Open]** C language standard, compiler and warning settings.

### 1.3 Platform

- **[Decided]** Linux only. Other operating systems are not addressed in this document.

### 1.4 Out of scope

- **[Decided]** Security: no permission system, no sandboxing, no protection against malicious plugins or files. Plugin and file contents are trusted.
- **[Decided]** Accessibility is not a priority.
- **[Decided]** Look and feel (themes, animations) is not designed.

---

## 2. Program structure

### 2.1 Modules

**[Proposed]** OpenECS is one executable built from these modules:

```
+----------------------------------------------------------------+
|  Plugin API (C context table + Lua `ecs` table)                |
+----------------------------------------------------------------+
|  Plugin host   | Script host | Service registry | Settings     |
|  Session       | Layout      | Input and focus  | Events       |
+----------------------------------------------------------------+
|  Render backend interface        |  Platform layer              |
+----------------------------------------------------------------+
|  SDL3, SDL3_ttf, (SDL3_image), Clay, Lua, libffi               |
+----------------------------------------------------------------+
```

| Module | Responsibility |
|---|---|
| Platform layer | OS windows, raw input, clipboard, dialogs, loading of shared libraries. The only place that talks to the operating system. |
| Render backend | Shows window surfaces on screen and draws the core's own interface parts. Behind a small fixed interface so it can be replaced (section 5.3). |
| Layout | Layout tree, workspaces, hit testing and drag-and-drop docking. Uses Clay to compute rectangles. |
| Input and focus | Turns raw input into focus changes, keybinding dispatch and window events. |
| Events | Creation and delivery of events; actions on windows. |
| Plugin host | Finds, orders, loads and unloads-at-exit plugins. |
| Script host | Owns the Lua state or states; runs all Lua code under protected calls. |
| Service registry | Names, signatures and handles for services (section 10). |
| Settings | Setting declarations, layers, explanation of values. |
| Session | Reading presets and sessions, applying them, serializing the current state. |

### 2.2 Boundary rule

- **[Decided]** No header that plugins include, and no function plugins call, exposes a type from SDL, Lua, Clay or libffi. These libraries are hidden behind the plugin API.
- **[Decided]** Anything beyond that wrapper is reached through a binding plugin that offers a library as a service.

### 2.3 Startup sequence

**[Proposed]**

1. Initialize the platform layer and the script host.
2. Read the command line and find the preset to use (details open, section 19).
3. Read the preset's manifest part (name, dependencies) without running plugin code.
4. Discover plugins; read their manifests; resolve dependencies; compute a load order.
5. Load each plugin in order and call its init function. Plugins register behaviours, services and settings.
6. Build the settings layers: core defaults, plugin defaults, preset, user file, generated file.
7. Apply the preset's session (create workspaces, layout, windows).
8. Enter the main loop.

### 2.4 Shutdown sequence

**[Proposed]** Leave the main loop; destroy windows; call each plugin's shutdown function in reverse load order; release services; shut down the script host; shut down the platform layer.

---

## 3. Main loop and threading

### 3.1 Loop

**[Proposed]** One iteration ("frame"):

1. Collect operating-system events from the platform layer.
2. Translate them: pointer events go through hit testing, key events go through key dispatch (section 7).
3. Deliver window events (section 8).
4. Call `update` on every window that is active, including windows in workspaces that are not visible and that are not asleep.
5. Recompute layout where needed.
6. For each visible window whose redraw policy requires it, call its draw function so it fills its surface.
7. Compose all surfaces and the core's own interface parts, and present.
8. Wait: sleep until the next frame or until an event arrives, depending on whether any visible window has the continuous redraw policy.

### 3.2 Threading rules

- **[Proposed]** All of these happen on the **main thread** only: OS window and input calls, layout, event delivery, every call into Lua, and every callback that a behaviour registers. SDL expects window and event calls on the main thread, and a Lua state must not be used by two threads at once.
- **[Proposed]** Every public API function documents which threads may call it. Most are main-thread only. A few are explicitly thread-safe (for example posting a message to the main thread, submitting background work, logging).
- **[Open]** The mechanism for background work: running a function on a worker thread and delivering its result to the main thread as an event. SDL3 offers threads and a function to run code on the main thread, which would be the basis (to be verified, see TODO).
- **[Open]** What native plugins may do on their own threads.

---

## 4. Windows and behaviours

### 4.1 Behaviour descriptor

**[Proposed]** A behaviour is registered by passing a descriptor to the core. Illustrative C form:

```c
typedef struct ECS_BehaviourDesc {
    uint32_t     struct_size;      /* sizeof this struct; see ABI rules, 9.5 */
    const char  *name;             /* behaviour name, for example "test"     */
    uint32_t     state_version;    /* version of this behaviour's saved state */
    ECS_RedrawPolicy redraw;       /* ECS_REDRAW_CONTINUOUS or ECS_REDRAW_ON_DEMAND */

    void *(*create)(ECS_Window *win, const ECS_Value *saved_state);
    void  (*destroy)(void *state);
    void  (*update)(void *state, double seconds);
    void  (*event)(void *state, const ECS_Event *ev);
    void  (*draw)(void *state, ECS_Surface *surface);
    ECS_Value (*save_state)(void *state);
} ECS_BehaviourDesc;
```

A Lua behaviour is a table with the same field names.

- **[Open]** Which functions are required and which are optional, and what `event` returns (see 8.3).
- **[Open]** Whether minimum size and similar hints belong in the descriptor.

### 4.2 Window instances

- **[Proposed]** Each window instance has a stable id, its behaviour, a state pointer owned by the behaviour, its current size, visibility, and redraw bookkeeping.
- **[Proposed]** Lifecycle: `create` (with saved state if restoring) → repeated `update`, `event`, `draw` → `destroy`.
- **[Decided]** Many instances can share one behaviour, each with its own state.

### 4.3 Redraw and sleep

- **[Decided]** Each window declares a redraw policy: continuous, or on demand.
- **[Proposed]** For on-demand windows the behaviour asks to be redrawn with a function such as `ECS_WindowInvalidate(win)`. The core also invalidates a window when its size changes.
- **[Decided]** Windows in workspaces that are not visible still receive `update` but are not drawn. A behaviour can declare that it sleeps, in which case it receives no `update`.
- **[Proposed]** Sleep is controlled with a function such as `ECS_WindowSetSleep(win, bool)`.

### 4.4 Registration point

- **[Proposed]** Behaviours are registered inside a plugin's init function with `ECS_RegisterBehaviour(ctx, &desc)`, and in Lua with `ecs.window.register_behaviour(desc)`.

---

## 5. Surfaces and drawing

### 5.1 What the core provides

- **[Decided]** The core gives each window a **pixel surface** and shows it. It offers **no high-level drawing commands**: no rectangle, text, image or widget functions for plugins.
- **[Decided]** High-level drawing and user-interface elements come from plugins, for example a drawing or UI plugin that offers a service.
- **[Proposed]** The core draws its own interface parts itself and does not expose them: tab rows, dividers, drag highlights and menus. Layout comes from Clay, text from SDL3_ttf.

### 5.2 Surface interface

**[Proposed]** Illustrative:

```c
typedef struct ECS_Surface {
    int      width, height;
    int      pitch;        /* bytes per row */
    void    *pixels;       /* writable for the duration of the draw call */
    ECS_PixelFormat format;
} ECS_Surface;
```

The core creates a surface per visible window, hands it to `draw`, then uploads and composes it.

- **[Open]** Pixel format, whether pixels are written directly or through a lock and unlock pair, and whether windows can report which part changed (dirty rectangles).
- **[Proposed]** When a window's size changes, its surface is recreated at the new size and the window gets a resize event.
- **[Decided]** A window moving to a different OS window can be given a new surface without noticing (for plain pixel windows).

### 5.3 Render backend interface

**[Proposed]** The framework talks to the backend through a small fixed set of functions, for example: create a target of a given size, destroy it, resize it, update its pixels, draw the core's interface parts from Clay's output, present a frame.

- **[Decided]** The backend is replaceable without breaking plugins, because plugins only see surfaces.
- **[Open]** Which backend is used: SDL's 2D renderer (simpler) or SDL_GPU (modern, can serve GPU-rendering windows).

### 5.4 GPU windows

- **[Open]** Whether the core offers windows a GPU-renderable surface, what that surface is, and what a window is told when it must be recreated. As far as is known, SDL renderer textures belong to one OS window's renderer, so a surface may need recreating when a window moves between OS windows (to be verified).

### 5.5 Drawing plugins

- **[Proposed]** A standard drawing plugin offers 2D functions (shapes, text, images) as a service that other plugins call to fill their surfaces. Tools that draw their own way (a terminal-style text grid, a 3D scene, direct pixel writes) do not need it.
- **[Open]** Which libraries a drawing plugin uses for text and images, given that plugins do not use SDL directly (see 16.2).

---

## 6. Layout

### 6.1 Data structures

**[Proposed]** Each OS window is a **root**: `{ OS window handle, tree root node, id of the maximized tab group (or none), geometry }`. A node has: `id`, `kind`, `parent`, and by kind:

| Kind | Fields |
|---|---|
| Split | direction (horizontal or vertical), ordered children, a fraction for each child (fractions sum to 1) |
| Tab group | ordered tabs (windows), index of the active tab |
| Window | the window instance |

**[Decided]** Every window is in a tab group, even alone.

A **workspace** is `{ name, list of roots }`. **[Decided]** Pop-out roots belong to the workspace in which they were created.

### 6.2 Tree invariants

**[Decided]** After every operation the tree is cleaned up:

- A split with one child is replaced by that child.
- A split directly inside a split of the same direction is merged into it.
- An empty tab group is removed.
- Node and window ids are stable and unique within a session.

**[Open]** Minimum sizes: whether windows can declare them, and how splits behave at the limit.

### 6.3 Operations

**[Decided]** Every change to the layout is a plain function, and every function has a Lua counterpart. Pointer gestures only compute arguments and call these functions.

**[Proposed]** Operations: split a window against a target on a side; move a window to a target and zone; close; pop out; swap; resize a divider; move focus in a direction; toggle maximize; switch workspace; add a tab; cycle tabs.

Example shape (illustrative): `ECS_LayoutMove(win_id, target_node_id, ECS_ZONE_LEFT)` and `ecs.layout.move(win_id, target_id, "left")`.

### 6.4 Pointer hit testing and drop zones

**[Decided]** OpenECS implements this itself. Clay computes rectangles only.

**[Proposed]** Algorithm while a window is dragged:

1. Find the deepest tab group whose rectangle contains the pointer.
2. Compute the pointer's position relative to that rectangle.
3. Choose the zone:
   - over the tab row: insert between tabs, using the midpoints of the existing tabs;
   - in the outer band of an edge (for example the outer quarter of the width or height): split in that direction;
   - otherwise (centre): add as a tab;
   - near the edge of the root: dock along the whole edge;
   - outside every OS window: pop out.
4. Draw a highlight for the zone; on release call the matching operation.

**[Open]** The exact band sizes and the handling of overlapping bands in small areas.

### 6.5 Use of Clay

- **[Decided]** Clay computes the layout. Clay is a pinned git submodule.
- **[Proposed]** One Clay context per OS window (Clay supports selecting a current context), recomputed when the layout or size changes. Clay's output is used for the core's own interface parts and to get the rectangle of each tab group and window.
- **[Proposed]** Because Clay's API has changed between versions, only one module (Layout) calls Clay.

### 6.6 Maximize and pop-out

- **[Decided]** Maximize marks one tab group as filling its root. Other windows stay alive but are hidden. The mark is saved in the session.
- **[Decided]** Pop-out creates a new OS window that holds a new root containing one tab group with the window. A pop-out root holds a full tree like the main root.
- **[Decided]** Pop-outs are hidden when their workspace is left and shown when it returns.
- **[Open]** On Wayland, placement of new OS windows is controlled by the compositor, so dragging out may not work; a menu entry and a key binding always do.

### 6.7 Workspaces

- **[Decided]** Switching is instant. Windows of the workspace that is left are not destroyed and keep receiving `update` (unless asleep) but are not drawn.
- **[Decided]** Workspaces are serializable.

---

## 7. Input, focus and keybindings

### 7.1 Focus

- **[Decided]** Exactly one window has focus. It changes by pointer hover, and by a keybinding with arrow keys that moves focus to the neighbouring window.
- **[Decided]** Focus is locked to a window during a pointer drag.
- **[Proposed]** Directional focus uses geometry: choose the nearest window in the given direction by rectangle overlap. It does not need to understand the tree.
- **[Open]** Details: pointer over gaps; whether only real pointer motion changes focus; windows that capture the pointer; focus across OS windows on Wayland.

### 7.2 Key dispatch order

**[Decided]** When a key combination is pressed, the first match wins in this order:

1. Keys reserved for the core.
2. The focused window instance.
3. The focused window's behaviour (all windows of that kind).
4. The workspace.
5. Global.

Workspace and global bindings belong to the core only. If nothing matches, the raw key event is delivered to the focused window.

### 7.3 Reserved keys

- **[Decided]** A set of key combinations is reserved for the core and is always checked first, **when the key is pressed**, not only when a binding is registered. Therefore a change of the reserved prefix at runtime cannot give plugins access to core keys. A plugin binding that now falls in the reserved set is not triggered and is reported.
- **[Proposed]** The reserved set is defined by a *core modifier*: a named setting that maps to a physical modifier key. Everything reserved is "core modifier plus something".
- **[Open]** The exact reserved set, the default modifier (Alt was used as an example), and interaction with keyboard layouts that type characters using Alt.

### 7.4 Binding keys

- **[Decided]** Keybindings are settings of the type "key combination" and follow the settings layers (section 12).
- **[Decided]** A plugin can bind keys only for windows or behaviours it registered. These bindings are active only while such a window has focus. Plugins have no function for creating workspace or global bindings.
- **[Proposed]** The setting holds the key combination, and the plugin supplies the function: `ECS_BindKey(ctx, behaviour, setting_name, fn)`.
- **[Decided]** The core handles only keys pressed together. No multi-key sequences in the core.
- **[Open]** Key representation: physical key position versus layout-dependent key meaning, modifier handling and encoding.

---

## 8. Events and actions

### 8.1 Events

**[Proposed]** An event has a type, the window id it concerns, a timestamp and a payload. Types include: window created, destroyed, resized, moved, focused, unfocused, shown, hidden, popped out, docked, tab changed, maximized, workspace switched, surface recreated; plus input events (key, pointer, text input) delivered to the focused window.

### 8.2 Delivery

- **[Proposed]** Events are delivered synchronously on the main thread to the window's own `event` function. A behaviour may also subscribe to events of its own windows.
- **[Proposed]** Destructive actions requested while an event is being delivered (closing a window, for example) are carried out after delivery finishes, so handlers never run on destroyed objects.
- **[Open]** Whether a plugin may subscribe to events of windows it does not own.

### 8.3 Refusing operations

- **[Open]** Whether a behaviour can refuse an operation (for example closing). If it can, events carry a flag saying whether they may be cancelled, and the handler returns a verdict. If it cannot, events are pure notifications and handlers return nothing. Related: a way for a window to report unsaved work.

### 8.4 Actions

- **[Decided]** Actions are plain functions called directly. There is no registry of named commands.
- **[Decided]** Each action has a Lua counterpart with the same meaning.

---

## 9. Plugin system

### 9.1 Layout on disk

**[Proposed]**

```
plugins/
  gltf/
    manifest.lua      -- description and dependencies
    plugin.so         -- native code (optional)
    init.lua          -- Lua code (optional)
```

- **[Decided]** Plugins are found by scanning a plugins directory. A manifest or preset can name another plugins directory.
- **[Proposed]** Paths inside a manifest are relative to the manifest file, not to the working directory.

### 9.2 Manifest

**[Proposed]** A manifest is a Lua file that returns a table:

```lua
return {
  name = "gltf",
  version = "1.0",
  api = 1,                                  -- plugin interface version it was built for
  description = "glTF model loader",
  plugins = { { name = "drawing", version = ">=1.0" } },   -- dependencies
  native = "plugin.so",                     -- optional
  lua = "init.lua",                         -- optional
}
```

- **[Decided]** Presets follow the same rules as plugin manifests: they can depend on plugins (see section 13).
- **[Proposed]** Manifests are evaluated without running plugin code, so that the list of available plugins can be shown without loading them.
- **[Open]** The exact fields and version-constraint syntax.

### 9.3 Discovery and load order

- **[Decided]** Plugins are loaded in dependency order, computed by the core. Plugin authors never manage order by hand.
- **[Proposed]** Build a dependency graph and order it topologically. Report missing dependencies, version mismatches and cycles clearly (section 14).

### 9.4 Native plugin interface

- **[Decided]** A native plugin exports a few well-known names that the core looks up when it loads the file: a query for the plugin interface version, an **init** function and a **shutdown** function. The version is checked before anything else is called. Everything else is registered explicitly inside init.
- **[Proposed]** Loading uses SDL3's functions for opening a shared library and looking up a symbol.
- **[Proposed]** `init` receives a **context** (`ECS_Context *`): a versioned table of function pointers belonging to that plugin. Plugins do not link against the core.
- **[Open]** The exact exported names and the header that defines them.

### 9.5 ABI rules for native plugins

**[Proposed]**

- A structure that crosses the boundary starts with its own size (`struct_size`) so the core can tell which fields exist.
- Structures only grow by adding fields at the end. Fields are never removed, reordered or changed in meaning.
- Structures are never passed or returned by value across the boundary; pointers are used.
- Enumeration values are never renumbered.
- Plain C types only. No C++ exceptions and no `longjmp` across the boundary.

### 9.6 Lua plugin interface

- **[Proposed]** The plugin's Lua entry file runs in an environment that has the global `ecs` table (section 11.4). It registers its behaviours, services and settings through that table.
- **[Open]** Whether all plugins share one Lua state with a separate environment each, or each plugin has its own state (section 11.2).

### 9.7 Lifecycle and activation

- **[Decided]** The active plugins are fixed at startup by the preset plus dependencies. No activating or deactivating plugins at runtime for now.
- **[Proposed]** Native plugins are never unloaded while the program runs (function pointers, threads and static state may still be in use). Lua plugins could be unloaded or reloaded if runtime activation is added.
- **[Decided]** Plugins use each other through services.

### 9.8 Built-in plugins

- **[Open]** Whether binding plugins for the core's own dependencies are compiled into the executable and registered like any plugin. This would keep a single copy of each dependency (16.2).

---

## 10. Services and function registration

### 10.1 Registration

**[Proposed]** Illustrative:

```c
ECS_RegisterFunction(ctx, "audio.play", audio_play, "int(string, float)");
```

```lua
ecs.service.register("audio", {
  play = { sig = "int(string, float)", fn = function(path, volume) ... end },
})
```

- **[Decided]** Functions are registered with a **declared signature**, and the declaration is validated at registration. An invalid declaration is rejected at that moment.
- **[Decided]** A plain C function can be registered without a hand-written wrapper.

### 10.2 Signature grammar and types

**[Proposed]** Signature strings describe the return type and parameter types. Supported types:

| Type | Meaning |
|---|---|
| `void` | No value (return only) |
| `bool` | true or false |
| `int`, `int64` | Whole numbers |
| `float`, `double` | Decimal numbers |
| `string` | Pointer to text ending in zero |
| `buffer` | A pointer together with a length |
| `handle<name>` | A typed handle (10.5) |
| `value` | A generic value (10.3) |

Structures by value are not supported; they are passed by pointer, as a handle or a buffer.

**[Open]** The final list of types, a limit on the number of parameters, and whether function-valued parameters (callbacks) are supported.

### 10.3 Values

**[Proposed]** A generic value is one of: nil, boolean, integer, number, string, buffer, handle, table (an ordered or named group of values). `ECS_Value` is the C representation. Values are what behaviours return from `save_state`, and what generic calls use.

### 10.4 How calls work

**[Proposed]**

- **Lua calls a native function:** the core creates one generic Lua-callable function per registered native function and attaches the function's descriptor to it (Lua supports attaching data to a C function). When called, it reads the arguments from the Lua stack, converts them according to the signature, calls the real function through libffi, and pushes the result.
- **Native calls a native function:** the caller asks for the function and receives the raw C function pointer with its signature, and calls it directly at full speed.
- **Native calls a Lua function:** through a generic call, `ECS_Call(fn, args, count, &result)`, which converts values, calls the Lua function under a protected call, and converts the result.
- **Lua calls a Lua function:** an ordinary Lua call.

**[Open]** Whether a Lua function can be given to native code as a raw C function pointer (libffi can create such callable pointers, but this adds complexity).

### 10.5 Handles

**[Proposed]**

- A handle stands for an object owned by its provider: a pointer plus a **type name** and a **destructor** registered with `ECS_RegisterHandleType`.
- In Lua a handle is a block of memory managed by Lua whose type is named by its metatable. Passing a handle of the wrong type is rejected with a clear error.
- When Lua no longer uses the handle, Lua's garbage collector calls the destructor.
- When a provider goes away, its handles become invalid; the core tells the users.

### 10.6 Buffers

- **[Proposed]** A buffer is a pointer plus a length. Native code reads it directly; Lua gets it as a typed array.
- **[Open]** The lifetime rule: documented per function (for example "valid until the handle is released"), and whether the core enforces anything.

### 10.7 libffi

- **[Decided]** libffi is used to call functions by declared signature. It is a git submodule.
- **[Proposed]** libffi is kept behind the service module only. Per function, the call description is prepared once at registration and reused for every call.
- **Limits that must be documented:** the core cannot check that a C function really matches its declared signature. A wrong declaration is a bug in the plugin and may crash.
- **[Open]** Behaviour on platforms and architectures other than the current Linux build.

### 10.8 Lookup and dependencies

- **[Proposed]** A plugin asks for a service by name and minimum version and receives a handle. The core allows this only for plugins named in the plugin's manifest dependencies, and informs users of a service if the provider goes away.

### 10.9 Namespaces

- **[Decided]** The prefix `ECS_` / `ecs.` belongs to the core.
- **[Open]** How plugins name what they register, so names cannot collide.

---

## 11. Lua integration

### 11.1 Version

- **[Decided]** Standard latest Lua.

### 11.2 State model

- **[Open]** One shared Lua state with a separate environment (table of visible globals) per plugin, or one Lua state per plugin. A per-plugin state isolates plugins better; a shared state makes passing values simpler.

### 11.3 Protected calls

- **[Proposed, to become a rule]** Every call from the core into Lua uses a protected call. Lua reports errors by jumping out of the current function, so an unprotected call could unwind through core code. A caught error becomes an error report (section 14), never a crash.

### 11.4 The `ecs` table

**[Proposed]** Layout of the Lua interface:

| Table | Contents |
|---|---|
| `ecs.window` | Behaviour registration, window actions and queries |
| `ecs.layout` | Layout operations |
| `ecs.workspace` | Workspace operations |
| `ecs.input` | Key binding, focus |
| `ecs.settings` | Declare, get, set, explain |
| `ecs.session` | Serialize, load |
| `ecs.service` | Register and look up services |
| `ecs.plugin` | Information about plugins |
| `ecs.log` | Logging |

### 11.5 Parity

- **[Decided]** Everything the core can do is also available from Lua.
- **[Proposed]** Keep a list of every public C function with its Lua counterpart, and check that list as part of the build or tests.

### 11.6 Storing and calling Lua values from C

- **[Proposed]** Lua functions and values that C code must keep are stored in Lua's registry and referenced by an integer; they are released when the owner is destroyed.

---

## 12. Settings

### 12.1 Declaration

- **[Proposed]** The core and plugins declare settings with a name, type, default value and description.
- **[Open]** The list of types. Planned: boolean, integer, number, string, choice, key combination, list, table.

### 12.2 Layers

**[Decided]** From lowest to highest priority; a higher layer overrides a lower one:

1. Core defaults.
2. Plugin defaults.
3. The preset.
4. The user's hand-edited settings file.
5. A generated file holding changes made through the settings window.

- **[Decided]** User settings override all other layers, including the preset.
- **[Decided]** The core never rewrites the hand-edited file.
- **[Decided]** Unknown keys (for example for a plugin that is not available) are kept, not dropped.
- **[Decided]** Keybindings are settings.

### 12.3 Interface

- **[Proposed]** `get(key)`, `set(key, value)` (writes to the generated layer), and `explain(key)`, which returns the value in effect, the layer it came from and what each layer says. The settings window uses `explain` to show locked (faded) settings with a hint.
- **[Proposed]** Owners are notified when their settings change.

### 12.4 Files

- **[Decided]** The user's hand-edited file may live under `~/.config`.
- **[Open]** File names, the location of the generated file, and how `$XDG_CONFIG_HOME` is resolved. As far as is known, SDL3's helper for per-user directories returns the data directory on Linux, not the configuration directory (to be verified).

---

## 13. Presets and sessions

### 13.1 Format

- **[Decided]** Presets and sessions are one file format: a Lua file following the same rules as a plugin manifest. It depends on plugins and returns a table describing a session: workspaces, each containing windows with their saved state.
- **[Decided]** A preset is hand-written and may contain logic. A session is generated by serialization. A saved session is a snapshot, not a link to its preset.
- **[Decided]** Lua is the format, so no separate parser is written.
- **[Proposed]** Illustrative:

```lua
return {
  name = "paint", version = "1.0",
  plugins_dir = "plugins",
  plugins = { { name = "canvas", version = ">=1.0" }, { name = "palette" } },
  settings = { ["canvas.grid"] = true },
  workspaces = {
    { name = "main",
      roots = { { layout = { type = "split", dir = "h", fractions = { 0.7, 0.3 },
                  children = { ... } } } } },
  },
}
```

- **[Open]** The exact fields and the file format version.

### 13.2 Applying a session

**[Proposed]** Steps:

1. Evaluate the file and check its shape (version, required fields, types); report errors with the path of the problem.
2. Make sure the plugins it needs are active.
3. Build the layout hidden: roots, splits with fractions, tab groups with the active tab, workspaces, the focused window, the maximized group.
4. Create each window: call its behaviour's `create` with the saved state.
5. Show everything at once.

**[Decided]** Rules:

- Nothing that cannot be understood is destroyed. If a behaviour is missing, a placeholder window is created that keeps the saved state unchanged, so saving writes it back.
- One failing window does not stop the rest. It falls back to a default state or an error placeholder.
- Each behaviour owns its state format and version; it is told which version it is reading. The core converts only layout data.

### 13.3 Serialization

- **[Decided]** There is a serialize action (button, and callable from Lua). It writes the current state to a file in a location other than the presets. There is no automatic saving.
- **[Proposed]** Only plain data is written: numbers, strings, booleans and tables. Functions and reference cycles are rejected with an error. Strings use Lua's own quoting so any content is stored correctly. Numbers are written so that reading them gives back the same value.
- **[Proposed]** The file is written to a temporary file and then renamed, so a failure never leaves a half-written file.
- **[Open]** Whether the writer is C code or a small embedded Lua function.

### 13.4 Loading and trust

- **[Decided]** Security is out of scope (1.4): files are loaded as ordinary Lua.

---

## 14. Error handling

### 14.1 Principles

- **[Decided]** A native plugin that crashes takes the whole program down; this cannot be isolated and is accepted.
- **[Proposed]** Everything else is contained: a Lua error or a failing plugin or window must not crash the core. Errors are collected as reports and shown, not hidden.
- **[Proposed]** An error report contains: the plugin, the kind of error, a message, and for Lua a stack trace.
- **[Proposed]** Reports go to the log and, for the user, to a message dialog (notifications are not available yet).

### 14.2 Policies by situation

**[Proposed]**

| Situation | Behaviour |
|---|---|
| Manifest cannot be read or is invalid | The plugin is skipped and reported. Its dependents are skipped. |
| Missing dependency or version mismatch | The plugin and its dependents are skipped and reported. |
| Dependency cycle | The plugins in the cycle are skipped and reported. |
| Plugin interface version mismatch | The plugin is refused before any of its code runs. |
| Init fails | The plugin is marked failed; what it registered so far is removed. |
| An invalid registration (bad signature, duplicate name, reserved key) | That registration is rejected and the error is returned to the plugin, which can continue. |
| A Lua callback raises an error | The error is reported, and the window is marked faulted according to a policy (see below). |
| Session restoring finds a missing behaviour or a failing window | Section 13.2. |

- **[Open]** The policy for a window whose callback raises errors: mark it faulted after the first error, or after a number of errors; what a faulted window shows; whether it can be retried.

### 14.3 Error interface

- **[Open]** C functions: return codes (for example an `ECS_Result` enumeration) plus a way to read the last error message. Lua: raise an error, or return nil plus a message.

### 14.4 Logging

- **[Open]** Levels, destination (terminal, file), file location, format.

---

## 15. Memory and ownership

- **[Proposed]** The rule is: whoever allocates frees, unless a function's documentation says otherwise. The core does not free memory a plugin allocated, and plugins do not free memory the core allocated.
- **[Proposed]** Strings passed into a function are valid for the duration of the call. A function that needs them longer copies them.
- **[Proposed]** Objects that cross plugins are handles with destructors (10.5). Buffers follow a documented lifetime (10.6).
- **[Open]** Whether the core provides an allocator for memory that crosses the boundary, so both sides use the same allocator.

---

## 16. Build and dependencies

### 16.1 Dependencies

| Dependency | Status |
|---|---|
| Lua | **[Decided]** core |
| Clay | **[Decided]** core, git submodule, layout |
| libffi | **[Decided]** core, git submodule, calling functions by signature |
| SDL3 | **[Decided]** core: OS windows, input, graphics output, clipboard, dialogs, drag and drop, threads, loading shared libraries |
| SDL3_ttf | **[Decided]** core: text for the core's own interface |
| SDL3_image | **[Open]** whether the core needs it (overview Q3) |
| SDL3_mixer, SDL3_net, cgltf | **[Decided]** not core; plugin dependencies |

### 16.2 One copy of SDL

SDL keeps global state, so two copies in one process conflict. SDL3_mixer and SDL3_net depend on SDL3 themselves.

- **[Open]** Link SDL3 dynamically in the executable so all libraries share one copy; and/or compile binding plugins for the core's dependencies into the executable (9.8).

### 16.3 Output

- **[Proposed]** One executable. Native plugins are separate shared libraries built against the plugin interface header.
- **[Proposed]** Build the executable with hidden symbols, exporting only what plugins need, so internal libraries cannot clash with a plugin's own.

### 16.4 Open build details

- **[Open]** Build system, C standard, compiler flags, how SDL3 and Lua are obtained (system package or submodule), how libffi is built.

---

## 17. Rules for contributors

### 17.1 For core developers

**[Proposed]** These rules apply to every change:

1. Never expose a type from SDL, Lua, Clay or libffi in a header that plugins include.
2. Every public function has a Lua counterpart (or a documented reason it does not).
3. Every public function documents its thread rule and the ownership of what it takes and returns.
4. Every call into Lua is a protected call.
5. Only the Layout module calls Clay; only the Service module calls libffi; only the Platform layer calls SDL.
6. Core code never checks a plugin's name to decide behaviour. Plugins get no special treatment.
7. If something is needed by the core's own machinery it is core; otherwise it is a plugin (overview, section 4.3).

### 17.2 For native plugin authors

**[Proposed]**

1. Check the interface version and register things only inside init.
2. Do not call the plugin API from threads other than those documented as allowed.
3. Do not free memory owned by the core, and do not keep core pointers beyond their documented lifetime.
4. No C++ exceptions and no `longjmp` across the boundary.
5. Declare function signatures accurately.

---

## 18. Platform notes

- **[Decided]** Linux only. SDL3 supports both X11 and Wayland.
- **[Open]** Wayland: the compositor decides where new OS windows appear and an application may not be able to force focus onto another of its own windows. Pop-out by dragging and focus across OS windows are therefore best effort; menu entries and keybindings must always work.
- **[Open]** Libffi build and behaviour on other platforms and architectures.

---

## 19. Open questions and TODO

### 19.1 Open questions

Each is also marked **[Open]** in the section named.

- **T1. Behaviour interface (4.1).** Which functions are required; what `event` returns; whether hints such as minimum size are in the descriptor.
- **T2. Surfaces (5.2).** Pixel format; writing pixels directly or through lock and unlock; dirty rectangles.
- **T3. Backend and GPU (5.3, 5.4).** SDL 2D renderer or SDL_GPU; GPU-renderable surfaces; telling a window its surface was recreated.
- **T4. Drawing plugin and SDL sharing (5.5, 9.8, 16.2).** Libraries for text and images in plugins; static or dynamic SDL; built-in binding plugins.
- **T5. Threading (3.2).** Background work mechanism; list of thread-safe functions; native plugin threads.
- **T6. Exported names and naming (1.1, 9.4, 10.9).** Exact names native plugins export; how plugins name what they register; the project owner's naming conventions.
- **T7. Services (10.2, 10.4, 10.6).** Final type list; parameter limit; callbacks; buffer lifetime; Lua functions as native pointers.
- **T8. Lua (11.2, 14.3).** One state or one per plugin; error convention for the Lua API.
- **T9. File formats and locations (9.2, 12.4, 13.1).** Exact fields of manifests, presets and sessions; version syntax; settings types; file names and directories; the command-line interface.
- **T10. Keys (7.3, 7.4).** Key representation; the reserved set and the core modifier; layouts that use Alt to type characters.
- **T11. Focus (7.1) and layout (6.2, 6.4).** Focus details; minimum sizes; drop-zone band sizes.
- **T12. Events (8.2, 8.3).** Subscriptions to events of windows a plugin does not own; whether handlers can refuse operations (overview Q2).
- **T13. Errors (14.2 to 14.4).** Policy for failing windows; C error convention; logging.
- **T14. Memory (15).** Shared allocator or not.
- **T15. Build (16.4).** Build system, C standard, flags, how dependencies are obtained.
- **T16. Settings (12.1, 12.4).** Type list; file names; location of the generated file.
- **T17. Serialization (13.3).** C writer or embedded Lua writer; file format version.
- **T18. Other platforms (18).** libffi and OS-window behaviour elsewhere.

### 19.2 TODO

- Verify assumptions about SDL3: whether renderer textures belong to one OS window and cannot be shared; which function gives a per-user directory on Linux and whether it is the data or the configuration directory; that desktop notifications are not offered; the functions for opening a shared library, looking up a symbol and running code on the main thread; GPU text support in SDL3_ttf.
- Verify how libffi behaves in the chosen build setup, including creating callable pointers for Lua functions.
- Receive and write down the naming conventions.
- Decide each open question above, remove its **[Open]** tag and write the decision into the section.
- Keep this document and the overview consistent whenever a decision is made.

---

## Appendix A: Technical terms

**ABI (application binary interface).** The rules for how compiled code fits together: where each field of a structure sits in memory, how large each type is, how functions receive arguments. Compiled code that disagrees about the ABI breaks even though no compiler complained.

**API (application programming interface).** The functions and types that one piece of code offers to another, as written in source code.

**Buffer.** A pointer to memory together with its length.

**Callback.** A function passed to other code so that it can be called later.

**Clay.** A layout library. It computes rectangles and produces drawing commands as data; it draws nothing itself.

**Context.** A table of function pointers that the core gives to a plugin, which is how a native plugin calls the core without linking against it.

**Destructor.** A function that releases an object when it is no longer needed.

**Dirty rectangle.** The part of a surface that changed and needs updating on screen.

**Environment (Lua).** The table of global names visible to a piece of Lua code. Giving code a restricted environment limits what it can see.

**Frame.** One pass of the main loop that updates and draws.

**Function pointer.** A value that holds the address of a function so it can be called through it.

**Handle.** A token that stands for an object owned by someone else, so it can be passed around without exposing its structure.

**Hit testing.** Finding which element is under a given point, such as the pointer.

**Invariant.** A condition that must be true every time a piece of data is at rest, for example "no split has only one child".

**libffi.** A library that calls a C function when only its description (signature) is known at run time.

**Lua stack.** The area through which C code and Lua exchange values: C pushes values onto it and reads values from it.

**Main thread.** The thread on which the program starts. Window and input calls and all Lua calls run there.

**Manifest.** A description file of a plugin or preset: name, version, interface version, dependencies.

**Metatable (Lua).** A table that defines how a Lua value behaves. For handles it carries the type name.

**Ownership.** The rule about which code is responsible for freeing an object.

**Protected call (Lua).** A way to call Lua code so that an error is caught and returned instead of jumping out through the caller.

**Registry (Lua).** A table Lua keeps for C code to store values it needs to keep alive.

**Root.** The top of a layout tree; one per OS window.

**Shared library.** A compiled code file that a program can load while it is running (`.so` on Linux).

**Signature.** A description of a function's parameter and return types.

**Struct (structure).** A group of named fields stored together in memory.

**Submodule (git).** A way to include another repository, at a fixed version, inside a repository.

**Surface.** A picture in memory that a window fills with pixels.

**Symbol.** A name in a compiled file that other code can look up, such as the name of an exported function.

**Topological order.** An ordering of items such that every item comes after the items it depends on.

**Userdata (Lua).** A block of memory managed by Lua's garbage collector that holds data from C. Handles are represented this way.