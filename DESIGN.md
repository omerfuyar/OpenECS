# OpenECS: Design Overview

**Status:** draft. The project is under development.

**Purpose of this document:** to explain, in plain language, what OpenECS is, what it covers, what it deliberately does not cover, and how it is meant to work. It is written so that a person with no background in the project, or an AI agent, can read it and understand the scope.

**What this document is not:** it is not the implementation specification. Function-level APIs, error handling rules and code-level conventions belong in a separate technical document (see the TODO list in section 16).

**How to read it:**

- Statements written as facts are decisions that have been made.
- Anything that is not decided is listed in section 16 ("Open questions and TODO"). Where a section touches an undecided point, it refers to the question by number, for example (Q4).
- Technical words are explained when they first appear and again in the glossary (Appendix A).

---

## Contents

1. Summary
2. What can be built with OpenECS
3. A walk-through from the user's point of view
4. Scope
5. Core vocabulary
6. Architecture overview
7. Windows and behaviours
8. Layout and workspaces
9. Input, focus and keybindings
10. Events and actions
11. Plugins
12. Services and registration
13. Presets, sessions and settings
14. Design principles
15. Dependencies
16. Open questions and TODO
- Appendix A: Glossary

---

## 1. Summary

OpenECS is a program, written in the C programming language, that provides the *empty shell* of an editor-style application.

Think of the layout of a modern editor or creative tool: panels that can be split side by side, stacked as tabs, dragged around, maximized, grouped into workspaces, and pulled out into their own operating-system windows. OpenECS provides exactly that machinery, plus keyboard shortcuts, saving and restoring of the arrangement, settings, and plugin loading.

**OpenECS itself does not know what any panel is for.** It has no idea what a text editor, a paint program or a game engine is. What appears inside each panel, and what it does, is supplied by **plugins**: small add-on modules written either in C or in the Lua scripting language. A plugin tells OpenECS "here is a new kind of window, and here is how it behaves".

Because of this, the same OpenECS program can become a text editor, a paint program, or the front end of a game engine, depending only on which plugins are loaded and how they are arranged.

OpenECS is distributed as an **executable** (a program you run), not as a library that other programs link against.

In one sentence: *OpenECS is the frame, and plugins are the pictures.*

## 2. What can be built with OpenECS

Examples of tools that could be assembled from OpenECS plus plugins:

- A text editor.
- A paint or drawing program.
- The editor front end of a game engine, with 3D views, asset browsers and property panels.
- A custom tool for any other specialized job.

None of these ship as part of the OpenECS core. Each one is a combination of plugins and a **preset** (a configuration file that says which plugins to use and how the windows are arranged; see section 13).

## 3. A walk-through from the user's point of view

This is a description of the intended experience, not a tutorial.

1. The user starts OpenECS and chooses a preset (for example one for painting).
2. OpenECS loads the plugins that the preset names, together with any plugins those plugins depend on.
3. A window opens, divided into areas. Each area has a tab row at the top. Each tab holds a **window** (a panel, such as a canvas or a colour palette) supplied by a plugin.
4. The user drags a tab to split the screen differently, stacks it with another tab, maximizes it, or pulls it out into a separate operating-system window.
5. The user switches between **workspaces**, which are saved arrangements of windows (for example "drawing" and "organizing files"). Switching is instant, and windows in the workspace that was left keep running in the background.
6. The user presses keyboard shortcuts. Some belong to OpenECS itself and always work. Others belong to whichever window currently has focus.
7. The user opens a settings window. Settings are layered, and a setting that a higher layer overrides is shown faded, with a hint about where to change it.
8. The user presses a button to **serialize** (save) the current arrangement and state as a session file, and can load it later.

## 4. Scope

### 4.1 What OpenECS provides

- Windows: tabs, docking, tiling, maximizing, pop-out into separate operating-system windows, and workspaces.
- Drawing support for windows, including text and images.
- Keyboard focus handling and keybindings.
- Events and actions for everything related to windows.
- A plugin system for native (C) plugins and Lua plugins.
- Services: a way for plugins to offer functions to other plugins and to scripts.
- Presets, sessions and settings, with saving and restoring.
- Operating-system integration provided by the platform library: for example drag and drop, copy and paste, and dialogs (see Q18 for exactly what is included).
- A scripting interface: **everything OpenECS can do is also available from Lua scripts.**

### 4.2 What OpenECS does not provide

OpenECS has **no domain-specific behaviour**. In particular, none of the following is part of the core. All of it comes from plugins:

- Text editing, painting, or any other tool logic.
- Audio playback and decoding.
- Networking.
- Loading and rendering 3D models.
- Media playback.

Some plugins are ideas for a **standard plugin** collection (plugins that are expected to be commonly useful): a glTF 3D model loader, networking, and audio with file loaders. How they are provided is not decided (Q28).

A test that decides whether something belongs in the core: *if OpenECS's own machinery (layout, focus, plugins, settings, drawing, sessions) needs it, it is core; if only some windows need it, it is a plugin.*

### 4.3 Target platform

The design is written for **Linux**. Support for other operating systems is intended but is not addressed in this document (Q23).

### 4.4 Deliberately deprioritized

Look and feel (themes, animations) is wanted but has not been designed. Functionality and structure take priority (Q20).

## 5. Core vocabulary

These are the words used throughout the document. Each is explained in everyday terms. The glossary in Appendix A has short definitions for more technical terms.

**Core.** The OpenECS program itself, without any plugins. It contains only the "frame" functionality listed in section 4.1.

**Window.** A rectangular area inside OpenECS's layout, with a tab in a tab row, whose contents are defined by a plugin. Note that this word does *not* mean an operating-system window; for that, this document says **OS window**.

**OS window.** A window as the operating system sees it: the thing with a title bar on your desktop. OpenECS can open several. Each OS window contains its own arrangement of OpenECS windows.

**Behaviour.** A description of *what a kind of window does*: what it shows, how it reacts to the mouse and keyboard, how it keeps its state. A behaviour has a name, for example `test`. Many windows can use the same behaviour, each with its own state. If a window is a room, the behaviour is the floor plan and the purpose of the room.

**Plugin.** A package that adds functionality to OpenECS. A plugin can provide behaviours (new kinds of windows), services (functions for others to call), and settings. A plugin is either **native** (compiled C code, delivered as a shared library file) or **Lua** (a script).

**To register.** To tell OpenECS, by calling one of its functions, that something exists. When a plugin registers a behaviour, it gives OpenECS the behaviour's name together with the functions OpenECS should call to make it work. Registration is how plugins connect to OpenECS: OpenECS never guesses what a plugin contains; the plugin announces it.

**Service.** A named group of functions that one plugin offers so that other plugins and scripts can use them. For example, an audio plugin could register a service called `audio` containing functions such as `play` and `stop`. A game plugin that needs sound asks OpenECS for the `audio` service and calls its functions. The caller does not need to know whether the service was written in C or Lua.

**Manifest.** A small description file that sits with a plugin (or with a preset). It states the plugin's name, version, which version of the OpenECS plugin interface it was written for, and which other plugins it depends on. OpenECS reads manifests to find out what exists and in what order to load things. A manifest is a Lua file.

**Dependency.** Something one piece of software needs in order to work. If plugin A depends on plugin B, OpenECS loads B first.

**Preset.** A hand-written file that describes a complete working setup: which plugins to use, how settings are configured, which workspaces and windows exist. Different presets turn the same OpenECS into different tools.

**Session.** The state of OpenECS at one moment: which plugins are in use, which workspaces exist, which windows are in each, and the saved state of each window. A session can be written to a file and loaded again. A preset and a session use the same file format; the difference is only in how the file was made (see section 13).

**Workspace.** A named arrangement of windows that can be switched to instantly, like a virtual desktop.

**Setting.** A named, typed value that configures the core or a plugin. Keybindings are settings.

**Event.** A notification that something happened (a window was resized, popped out, docked, and so on).

**Action.** A function that makes something happen (close a window, pop one out, maximize it).

**Focus.** The one window that currently receives keyboard input.

## 6. Architecture overview

```
+--------------------------------------------------------------+
|  Plugins: native C shared libraries and Lua scripts          |
|  (behaviours, services, settings, supplied by plugin authors)|
+--------------------------------------------------------------+
|  Plugin API: the only way plugins interact with OpenECS      |
+--------------------------------------------------------------+
|  Framework logic                                             |
|  layout tree, workspaces, focus, input dispatch, events,     |
|  settings, presets and sessions, plugin loading              |
+------------------------------+-------------------------------+
|  Rendering backend           |  Platform layer               |
|  (turns drawing commands     |  (OS windows, input,          |
|   into pixels; replaceable)  |   clipboard, dialogs)         |
+------------------------------+-------------------------------+
|  Dependencies: SDL3, SDL3_image, SDL3_ttf, Clay, Lua, libffi |
+--------------------------------------------------------------+
```

Three rules shape this layering:

1. **Plugins talk only to the plugin API.** They never see the libraries underneath. A plugin never receives an SDL object, a Lua interpreter handle, or a Clay structure through the normal API. If a plugin needs access to one of those libraries, it depends on a separate **binding plugin** that offers the library as a service.
2. **The rendering backend is replaceable.** The framework logic only talks to the backend through a small fixed interface, so the way pixels are produced can change without breaking any plugin (Q4).
3. **Platform-specific details stay in one place**, the platform layer.

## 7. Windows and behaviours

**What a window is.** A window is a box in OpenECS's layout. OpenECS decides where the box is and how large it is, and whether it is visible or focused. The window's behaviour decides everything inside the box.

**Behaviours can do anything inside the box.** A behaviour can run any code and react to events. Examples of what a behaviour might show: a plain colour fill, a text display like a terminal, a 3D scene rendered on the graphics card, or an image whose pixels it controls directly. OpenECS does not need to know which.

**How behaviours are made available.** A plugin registers a behaviour under a name. The same registration mechanism is used by native plugins and by Lua plugins.

**What a behaviour registers.** A behaviour hands OpenECS a set of functions (for example one that draws, one that handles events, one that does periodic work). The exact set has not been decided (Q2).

**How windows are opened.** A window of a given behaviour can be opened from a menu, from a keyboard shortcut, or by a workspace preset.

**Drawing.** Every window draws into its own **off-screen surface** (a private picture that is not yet on screen), owned by OpenECS. OpenECS then assembles these surfaces into what the user sees. The consequences:

- One window cannot paint over another.
- OpenECS can move, scale or fade windows without their cooperation.
- A window that moves into another OS window can be given a new surface without the window noticing.

Drawing is abstracted: plugins describe what to draw through OpenECS's own drawing interface, and OpenECS turns that into pixels through the rendering backend. The exact drawing commands, and whether windows can render directly with the graphics card, are open (Q3, Q4).

**Redraw policy.** Each window declares how often it needs to be drawn. A game-like window may ask to be redrawn every frame (**continuous**). A text-like window may ask to be redrawn only when something changed (**on demand**). This avoids wasting power on idle windows.

**Background behaviour.** Windows in a workspace that is not on screen keep receiving their periodic updates, but are not drawn. A behaviour can declare that it wants to **sleep** (pause) instead.

**State.** Each window has its own state. Because sessions are saved and restored, a behaviour needs a way to save and load its own state. The format of a behaviour's state belongs to that behaviour, and the behaviour records a version for it so it can read older saves.

## 8. Layout and workspaces

### 8.1 The layout tree

Every OS window is a **root** that holds one **layout tree**. A tree has three kinds of node:

- **Split:** divides an area horizontally or vertically among its children. The children's sizes are stored as fractions of the whole, so resizing keeps proportions.
- **Tab group:** an ordered row of tabs, with one tab active.
- **Window:** one behaviour instance, as described in section 7.

**Every window always lives in a tab group, even if it is the only one.** The tab row (the header) is the place where the user drags the window and opens its right-click menu.

### 8.2 Layout is computed with a library; docking is OpenECS's own

The sizes and positions of areas are computed by **Clay**, a layout library. The layout tree, the docking logic, and the handling of pointer drags (working out what the pointer is over and where a dragged window should land) are written as part of OpenECS.

### 8.3 Operations are plain functions

Everything that changes the layout is a function: split, move a window to a tab group, close, pop out, swap, resize a divider, move focus, maximize, switch workspace. The mouse only supplies the arguments. A drag works out a *target* and a *zone* and then calls the same function that a script would call. This keeps the tree logic separate from mouse handling and gives Lua the same powers.

### 8.4 Drag-and-drop docking

While a window is dragged, OpenECS finds the tab group under the pointer and uses the pointer's position inside it to pick a landing zone:

| Pointer position | Result |
|---|---|
| Centre of a tab group | Add the window as a tab. |
| Near an edge of a tab group | Split in that direction. |
| Over a tab row | Insert at that position between tabs. |
| Near the edge of the whole root | Dock along the whole edge. |
| Outside the OS window | Pop out into a new OS window. |

A highlight shows the zone while dragging, and the move happens on release. On Wayland (a Linux display system), the compositor decides where new OS windows go, so dragging out to pop out may not work there; a menu entry or a keybinding is always available as an alternative.

### 8.5 Keeping the tree tidy

After each operation, OpenECS restores these rules:

- A split with a single child is replaced by that child.
- A split nested directly in a split of the same direction is merged into it.
- An empty tab group is removed.
- Every node and every window has a stable identifier, so sessions can refer to them.

A behaviour may declare a **minimum size** so that splits cannot shrink it to nothing.

### 8.6 Maximize

Maximizing marks one tab group as filling the whole root. The other windows stay alive but hidden. The mark is part of the saved session.

### 8.7 Pop-out windows

A window that is "popped out" becomes its own **OS window**. This is the only way for a window to leave the main OS window, because a program cannot draw outside its own OS windows. A pop-out holds a full layout tree of its own, so windows can be split and tabbed inside it exactly as in the main OS window. Floating panels that overlap windows *inside* the main OS window are not part of the design.

Pop-outs **belong to their workspace**: they are hidden when the user leaves the workspace and shown when the user returns.

### 8.8 Workspaces

A workspace is a named arrangement of windows (and of pop-outs). Workspaces can be saved and restored. Switching is instant because nothing is rebuilt: the windows of the workspace that was left keep existing and keep running in the background (see section 7).

### 8.9 Right-click menus

Windows and tab rows have right-click menus with entries such as close, pop out and add tab. Who may add entries is open (Q12).

## 9. Input, focus and keybindings

### 9.1 Focus

Exactly one window has focus at any time. Focus is decided in two ways:

- **Pointer hover:** the window under the pointer gets focus.
- **A keybinding with arrow keys** moves focus to the neighbouring window.

**Focus stays on a window during a pointer drag**, even if the pointer crosses other windows.

Further details are open (Q11).

### 9.2 Keybindings are settings

A keybinding is a setting of the type "key combination". It therefore follows the same layering rules as every other setting (section 13.4). Rebinding keys once changes them for every preset.

### 9.3 Scopes and precedence

When a key is pressed, these scopes are consulted, **most specific first**, and the first match wins:

1. The core's reserved keys (see 9.4). These are always checked before anything else.
2. The focused window.
3. The focused window's behaviour (all windows of that kind).
4. The workspace.
5. Global.

Workspace and global keybindings belong to the core only.

**Plugins can only bind keys for windows that they themselves provide, and those bindings are only active while such a window has focus.** Plugins cannot create global keybindings. A plugin that wants the same key for several of its own windows records that in a shared settings file.

If no binding handles a key press, the key event is delivered to the focused window, so a window can implement its own input logic (for example modal editing) without involving the core.

### 9.4 Keys reserved for the core

A set of key combinations is reserved for the core. This also marks the line between core and plugin behaviour. As an example, any combination that starts with the Alt key could be reserved; the exact rule is open (Q10).

Because reserved keys are checked *when a key is pressed*, and not only when a plugin registers a binding, the core always wins. If the reserved prefix is changed while OpenECS is running, plugins cannot gain access to core keys: a plugin binding that now collides with the reserved set is simply not triggered.

### 9.5 No key sequences in the core

The core only handles keys pressed together (for example Ctrl+S), not sequences pressed one after another (like Emacs chords). A window may implement sequences inside itself.

## 10. Events and actions

**Events** tell code that something happened. **Actions** are functions that make something happen. For windows there are events and actions for anything that matters: created, resized, moved, focused, popped out, docked, tiled, maximized, hidden or shown by a workspace switch, closed.

Rules:

- **No behaviour can refuse a window operation.** A plugin cannot stop a window from being closed, maximized, popped out, and so on. Events are notifications only. How a window with unsaved work should signal this to the core is open (Q7).
- **Actions are plain functions that are called directly.** There is no separate layer of named commands between code and functionality.
- **Everything is available from Lua.** Anything OpenECS can do can also be done from a Lua script: registering behaviours, binding keys to windows, serializing a session, and so on.

## 11. Plugins

### 11.1 Kinds of plugin

- **Native plugin:** compiled C code in a **shared library** (a file with the extension `.so` on Linux that a program can load while it is running). It has more control and speed, and in exchange must follow written contracts about how it is built and how it behaves (to be defined in the technical document).
- **Lua plugin:** a script. Easier to write and restricted by what OpenECS gives it.

The two kinds are **interchangeable**: a service written in Lua can be called from a native plugin, and the other way round. A native function can be called from Lua without anyone writing extra wrapper code (see section 12).

Plugins are used for essentially anything domain-specific.

### 11.2 Where plugins live and how they are found

Plugins are placed in a plugins directory. Each plugin is a folder holding its manifest and its code (a shared library, a Lua file, or both). OpenECS scans the plugins directory, reads the manifests, works out the order in which to load things from their dependencies, and loads them. A manifest or preset can name a different plugins directory, and directory locations are meant to be configurable.

Paths written inside a manifest are relative to the manifest file itself, not to where OpenECS was started.

### 11.3 How a native plugin connects

A native plugin exports a small number of fixed, well-known names that OpenECS looks for when it loads the file:

- A name that reports which version of the plugin interface it was built for. OpenECS checks this *before* calling anything else, so an incompatible plugin is refused cleanly instead of crashing.
- An **init** function. OpenECS calls it once. Inside it the plugin registers all its behaviours, services and settings.
- A **shutdown** function.

Fixed names are used only for these once-per-plugin steps. Everything else (behaviours, services) is registered explicitly inside init. This is because one plugin may offer many behaviours, and fixed names could only describe one.

### 11.4 Dependencies and loading order

A plugin's manifest lists the plugins it depends on. OpenECS loads plugins in dependency order, reports missing dependencies and circular dependencies clearly, and plugin authors never manage order by hand.

### 11.5 Which plugins are active

The active plugins are decided when OpenECS starts: those named by the preset, together with their dependencies. Enabling or disabling plugins while OpenECS runs is not supported. The design aims to leave room for it (Q15).

### 11.6 Trust

A native plugin runs inside the OpenECS program with full access to it; it is trusted. A Lua plugin runs in an environment that OpenECS controls and limits. Plugin permissions and further details are open (Q8).

## 12. Services and registration

### 12.1 Registration

As defined in section 5, to register means to tell OpenECS that something exists by calling one of its functions. Plugins register behaviours, services, settings and keybindings.

### 12.2 Services

A plugin makes functionality available to others by registering a service. The caller asks OpenECS for the service by name, and OpenECS gives it a handle to use. Because OpenECS stands in the middle, it can check that a plugin declared the dependency in its manifest, and it can tell users of a service if its provider goes away.

### 12.3 Declared signatures

When a function is registered as part of a service, the registering plugin also **declares its signature**: a short description of what the function takes and returns, for example "takes a piece of text and a decimal number, returns a whole number". OpenECS checks the declaration when the function is registered. Declarations that make no sense (unknown types, unsupported shapes) are rejected right then, not later when somebody calls the function.

With the declared signature, OpenECS can convert values between Lua and C on its own and call the real C function. This is what makes it possible for Lua to call a plain C function without any hand-written wrapper. The conversion relies on **libffi**, a small library that can call a C function when only its description is known at runtime.

What registration cannot check: whether the C function really matches its declared signature. A wrong declaration is a bug in the plugin that made it.

Details of supported types, buffers, handles and lifetimes are open (Q14).

### 12.4 Dependencies are never exposed

The API that plugins use does not expose OpenECS's own dependencies (SDL, Lua, Clay, libffi). OpenECS wraps them. Anything beyond what OpenECS wraps is reached through a binding plugin that offers the library as a service (for example, one that wraps a specific library for scripts).

## 13. Presets, sessions and settings

### 13.1 Presets and sessions

A **preset** and a **session** are files in the same format: a Lua file that follows the same rules as a plugin manifest. It has a name and version, depends on plugins, and returns a table describing a session: **workspaces**, each holding **windows** with their saved state.

- A **preset** is written by hand. Because it is a Lua file, it can contain logic (for example loops or conditions) to build its result.
- A **session** is generated: pressing the **serialize** button (also available as an action from Lua) writes the current state to a file in a location other than the presets, so a hand-written preset is never overwritten.
- A saved session is a *snapshot* of the state, not a link back to the preset it started from.
- Presets in one directory can share one plugins directory, so one set of plugins can serve many presets.
- There is no automatic saving (Q21).

Lua is used as the file format because OpenECS already contains a Lua interpreter, so no separate file parser has to be written.

Which restrictions apply when such files are loaded is open (Q9, Q27).

### 13.2 Applying a session

When a session is loaded, OpenECS creates the layout, creates each window, and gives each window its saved state. These rules apply:

- **Nothing that cannot be understood is destroyed.** If a window's behaviour is not available (for example its plugin is missing), OpenECS shows a placeholder window and keeps the window's saved state untouched, so that saving again writes it back.
- **One bad window does not stop the rest.** If restoring a window fails, that window falls back to a default state or an error placeholder, and everything else loads.
- **Every behaviour owns its state format.** The behaviour declares a version for its state and is told which version it is reading, so it can convert older data. OpenECS only converts the layout parts.
- **Saving is safe.** A file is not left half-written if something goes wrong while saving.

### 13.3 Where files live

On Linux, user-level settings may live under the user's configuration directory (`~/.config`). Generated sessions are written to a location other than the presets. The layout of directories is configurable; for example, a manifest can name its plugins directory (Q16).

### 13.4 Settings

Settings are **layered**. Each later layer overrides the layers before it. From lowest to highest:

1. Core defaults.
2. Plugin defaults, declared by each plugin when it registers a setting (name, type, default value, description).
3. The preset.
4. The user's hand-edited settings file.
5. A generated file that holds changes made through the settings window.

**The user's settings override everything else**, including the preset.

Rules and behaviour:

- Settings have types, so values can be checked, and a wrong value in a hand-edited file produces a clear message.
- A setting that belongs to a plugin that is not currently available is kept in the file, not removed.
- The core never rewrites the hand-edited file. Changes made in the settings window go to the separate generated file, which avoids destroying the user's comments and formatting.
- The settings window shows a setting as **faded (locked)** if a higher layer overrides it, with a **hint** pointing to the layer or file where it can be changed. To support this, the settings system can explain a value (the value in effect and which layer it came from), not only return it.
- Keybindings are settings.

## 14. Design principles

1. **Shallow core.** The core provides mechanisms (how things work). Policies (what is shown, which keys do what) come from plugins and settings. Domain-specific functionality never goes into the core.
2. **Everything is available from Lua.** The scripting interface can do everything the C interface can do.
3. **Least authority.** A plugin can act only through what it was handed. For example, a plugin can bind keys for its own windows only, and has no function for creating global keybindings at all.
4. **One front door.** All interaction between plugins and the core goes through the plugin API. The core decides at the moment something is used, not only when it is registered (for example, the reserved-key check in section 9.4).
5. **Small, uniform API.** Few concepts, consistently applied, are easier to learn, keep stable, and expose to Lua.
6. **Hide dependencies.** Plugins never see SDL, Lua, Clay or libffi directly, so those can be changed without breaking plugins.
7. **Design for replacement.** Places that are likely to change, such as the rendering backend, sit behind small fixed interfaces.
8. **Do not lose what is not understood.** Unknown settings, missing behaviours and unreadable window state are preserved, not discarded.
9. **Errors do not take the whole program down.** A broken script or window affects only itself (specified in the technical document, Q17).

## 15. Dependencies

| Library | Used for |
|---|---|
| Lua | The scripting language for plugins, presets, sessions and settings files. |
| Clay | Computing the layout (sizes and positions) of the areas on screen. |
| SDL3 | Creating OS windows, receiving input, graphics output, clipboard, dialogs, drag and drop, threads, and loading shared libraries. |
| SDL3_ttf | Text rendering. |
| SDL3_image | Loading images. |
| libffi | Calling functions in plugins by their declared signature. |

Libraries that are **not** part of the core and are expected to be used by plugins: SDL3_mixer or another audio library, SDL3_net or another networking library, and cgltf (a glTF 3D model reader).

How SDL3 is shared between OpenECS and plugins that depend on it is open (Q5).

---

## 16. Open questions and TODO

Questions are not decided. Each is listed with the context needed to answer it.

### Open questions

**Q1. What does "ECS" in the name stand for?** This document does not define it. If it is meant to be read as "entity component system", that is not an architecture used in this design as described here.

**Q2. Which functions does a behaviour register?** Candidates under consideration: create, destroy, draw, handle event, periodic update, save state, load state. It is not decided which are required and which are optional, or how redraw policy and sleep are declared.

**Q3. What are the drawing commands?** The set of 2D commands (rectangles, text, images, clipping), how a window uploads pixel data, and whether helpers such as a text-grid (terminal-style) display exist in the core or in plugins.

**Q4. Rendering backend, and GPU windows.** Two options were discussed: SDL's 2D renderer, which is simpler, and SDL_GPU, which is the modern graphics API in SDL3 and can serve windows that render 3D scenes themselves. It is not decided which to use or whether "raw GPU" windows (windows that issue their own graphics-card commands) are supported from the start. Related: a surface may have to be recreated when a window moves to another OS window; whether windows are told about this is undecided. Some SDL3 details assumed during design still need to be verified (see TODO).

**Q5. One copy of SDL.** SDL keeps global state, so two copies in one process conflict. SDL3_mixer and SDL3_net plugins depend on SDL3 themselves. Options: link SDL3 dynamically in the executable so all libraries share one copy; and/or build binding plugins for the core's own dependencies into the executable. Not decided.

**Q6. Threading.** Proposed: everything involving OS windows, input, layout and Lua runs on the main thread, with a mechanism for running background work whose result is delivered back to the main thread. Not decided, nor what is allowed in native plugins.

**Q7. Unsaved work.** Since behaviours cannot refuse to close, how does a window with unsaved changes say so? An idea: a "dirty" flag that the core reads and uses to decide whether to ask the user. Not decided.

**Q8. Plugin permissions and trust.** What a manifest can ask for, who grants it, and how a very powerful capability (for example a generic binding plugin that lets scripts call arbitrary native libraries) is controlled.

**Q9. Safety when loading files.** Proposed: files in the presets directory are trusted like an initialization script; generated sessions and files from elsewhere are evaluated in a restricted environment (no file or operating-system access, no compiled bytecode, limits on how long they may run). Not decided.

**Q10. Reserved keys.** The exact set, the default for the core modifier (Alt was used as an example), whether the modifier is a named setting that can be changed, and how this interacts with keyboard layouts that use Alt-based combinations to type characters.

**Q11. Focus details.** What happens when the pointer is over a gap or border; whether only real pointer movement changes focus (not layout changes under a still pointer); how a window that captures the pointer (for example a 3D view) interacts with focus; how focus moves between several OS windows on Wayland, where the application may not be able to force it.

**Q12. Right-click menus.** The default entries, and whether and how a behaviour can add entries for its own windows.

**Q13. Naming.** Proposed: each plugin owns a name prefix for everything it registers and the core owns a reserved prefix, so names cannot collide. Not decided.

**Q14. Service calls in detail.** Which value types can cross the boundary between plugins; how pointers, structures and large buffers are passed (for example as typed handles and buffers); who owns and frees them; how services written in Lua are called from native code; what happens on errors.

**Q15. Activating and removing plugins at runtime.** Not supported for now. Open: how far the design should prepare for it. Native plugins cannot be safely unloaded while running, so reloading may apply only to Lua plugins.

**Q16. Launching and file locations.** The command-line interface (for example how a preset is chosen), the exact directories for presets, sessions, plugins and settings, and the names of the files.

**Q17. Error handling and diagnostics.** What happens when a plugin fails to load, when a script raises an error, or when a native plugin misbehaves; logging; how problems are shown to the user. To be defined in the technical document.

**Q18. Operating-system integration.** Exactly which of these are included in the core: clipboard, drag and drop, file dialogs, message dialogs, system tray. Desktop notifications are not part of the current design; whether they will be in-application, native to the operating system, or a plugin is open.

**Q19. Undo and redo.** Wanted, but not designed. Open: whether history is per window or global, and how plugins take part.

**Q20. Look and feel.** Themes and animations. Not designed.

**Q21. Automatic saving and crash recovery.** Not part of the current design. Only manual serialization exists.

**Q22. A library form of OpenECS.** Not part of the current design; a version that other programs link against may be considered later.

**Q23. Other operating systems and processor architectures.** Not addressed. In particular, building libffi and handling OS windows differ across platforms.

**Q24. Which Lua.** Standard Lua 5.4 or LuaJIT (faster, but based on older Lua 5.1 semantics).

**Q25. Accessibility and text input.** Windows are drawn by OpenECS rather than by the operating system, so screen-reader support and input methods for complex scripts need explicit consideration.

**Q26. Layout details.** Whether tab rows offer further behaviours (reordering rules, scrolling of long rows), the default minimum sizes, and what the tab row shows.

**Q27. File format details.** The exact fields of presets, sessions and manifests, and how their format versions are handled.

**Q28. Standard plugins.** Which ones exist, where they live, and how they are distributed.

### TODO

- Write the technical document: the plugin API for C and Lua, the exact behaviour interface, drawing commands, error handling, threading rules, file formats and the contracts native plugins must follow.
- Verify details of SDL3 that were assumed during design: sharing of graphics resources between OS windows, which operating-system integration it offers on Linux (especially notifications), and which directory function applies to configuration versus data.
- Verify how libffi behaves in the target build setup.
- Decide each open question above and move the answer into the relevant section.

---

## Appendix A: Glossary

**Action.** A function that makes something happen, such as closing or maximizing a window.

**Behaviour.** A named description of what a kind of window shows and does. Many windows can share one behaviour.

**Binding plugin.** A plugin whose purpose is to make a library usable by other plugins as a service.

**Clay.** A layout library. It computes the sizes and positions of rectangles on screen, and produces drawing commands as data, without drawing anything itself.

**Compositing.** Assembling several pictures (here, the off-screen surfaces of windows) into the one picture shown on screen.

**Core.** OpenECS without plugins.

**Dependency.** Something a piece of software needs in order to work, such as a library or another plugin.

**Docking.** Attaching a window to an area of the layout, by splitting an area or adding a tab.

**Event.** A notification that something happened.

**Executable.** A program that can be run directly, as opposed to a library that other programs link against.

**Focus.** The one window that currently receives keyboard input.

**Handle.** A token that stands for an object owned by someone else, such as a service or a loaded model, so that it can be passed around without exposing its inner structure.

**Init.** The function a plugin exports that OpenECS calls once after loading it. The plugin registers everything it provides inside it.

**Keybinding.** An association between a key combination and a function. In OpenECS it is a setting.

**Layout tree.** The structure of splits, tab groups and windows inside one OS window.

**libffi.** A library that can call a C function at runtime when only the function's description (its signature) is known.

**Lua.** A small scripting language, used here for plugins and for presets, sessions and settings files.

**Manifest.** A description file that gives a plugin's (or preset's) name, version, interface version and dependencies.

**Native plugin.** A plugin made of compiled C code in a shared library.

**Off-screen surface (render target).** A picture that a window draws into that is not directly on screen; OpenECS later assembles these into the screen picture.

**OS window.** A window as seen by the operating system, with its own title bar. Contains one layout tree.

**Plugin.** A package that adds behaviours, services or settings to OpenECS.

**Pop-out.** Moving a window out of its OS window into a new OS window of its own.

**Preset.** A hand-written file that describes a working setup: plugins, settings, workspaces and windows.

**Redraw policy.** A window's declaration of whether it needs to be redrawn continuously or only on demand.

**Register.** To tell OpenECS, by calling one of its functions, that something (a behaviour, service, setting or keybinding) exists.

**Rendering backend.** The replaceable part of OpenECS that turns drawing commands into pixels.

**Root.** The top of a layout tree, one per OS window.

**Sandbox (restricted environment).** A running environment from which code cannot reach files, the operating system or other dangerous functionality.

**SDL3.** A cross-platform library for windows, input, graphics, audio and many system functions. SDL3_ttf adds text rendering, SDL3_image adds image loading, SDL3_mixer adds audio mixing, SDL3_net adds basic networking.

**Service.** A named group of functions a plugin offers to other plugins and scripts.

**Session.** The saved state of OpenECS: plugins in use, workspaces, windows and their states.

**Setting.** A named, typed value that configures the core or a plugin.

**Shared library.** A compiled code file that a program can load while it is running (extension `.so` on Linux).

**Signature.** A short description of the inputs and outputs of a function.

**Sleep.** A behaviour's request to pause its periodic work while it is not visible.

**Split.** A layout node that divides an area horizontally or vertically among its children.

**Standard plugin.** A plugin that is expected to be generally useful, such as a 3D model loader, an audio plugin or a networking plugin. Not part of the core.

**Tab group.** A layout node holding an ordered row of tabs, one of which is active.

**Tiling.** Arranging windows side by side without overlap.

**Update.** The periodic work a window does, independent of drawing.

**Window.** A rectangular area of OpenECS's layout whose contents are defined by a behaviour. Not the same as an OS window.

**Workspace.** A named arrangement of windows that can be switched to instantly.