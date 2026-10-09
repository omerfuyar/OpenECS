# OpenECS Overview

This document explains what OpenECS is, what it covers, what it leaves to plugins, and how it behaves for the user.

**How to read it:**

- Everything here is decided. Open questions and pending work are in [TODO.md](TODO.md).
- How OpenECS is built (interfaces, file formats, rules for code) is in [DESIGN.md](DESIGN.md).
- Terms used in this document are explained in the glossary at the end.

---

## Contents

1. Summary
2. The user's view
3. Scope: core and plugins
4. Architecture
5. Panels
6. Layout and workspaces
7. Keys and focus
8. Plugins
9. Plugin communication
10. Presets, sessions and settings
11. Operating-system integration
12. Principles
- Glossary

---

## 1. Summary

OpenECS is a program, written in C, that provides the empty shell of an editor-style application. Think of a modern editor or creative tool: areas that can be split side by side, grouped into tabs, maximized, moved into their own windows and arranged into workspaces. OpenECS provides that machinery, plus keybindings, settings, plugin loading, saving and restoring of sessions.

OpenECS does not know what any area is for. It has no idea what a text editor or a paint program is. What each area shows and does comes from **plugins**: add-on modules written in C or in Lua.

So the same program becomes a text editor, a paint program or the front end of a game engine, depending on which plugins and **preset** it loads.

OpenECS is an executable, not a library.

## 2. The user's view

1. The user starts a tool, for example "Paint" from the desktop's application menu. This runs OpenECS with the paint preset. Started without a preset, OpenECS shows a launcher that lists presets and saved sessions.
2. OpenECS loads the preset's plugins and the plugins they depend on.
3. If the user used this tool before, OpenECS restores the arrangement from last time. Otherwise it builds the arrangement the preset describes.
4. The window is divided into **panels**, such as a canvas, a colour palette or a layer list. Panels grouped in the same place show a **tab row**; a panel on its own shows none.
5. The user drags panels to split the window differently, groups them as tabs, maximizes one, or moves one into its own window, popping out from main window.
6. The user switches between **workspaces**, for example "drawing" and "organizing". Switching is instant.
7. The user presses keys. The focused panel's keys work, the core's own actions are always reachable, and the user's own keybindings win over everything else.
8. The user opens the settings window. A setting that is overridden elsewhere is shown faded, with a hint that says where it is set.
9. When the user quits, OpenECS asks what to do with unsaved work, then remembers the arrangement for next time. The user can also save sessions to files and open them later.

## 3. Scope: core and plugins

The most important rule of OpenECS is the line between the **core** (OpenECS itself) and **plugins** (everything else). The core is small. It provides the machinery of an editor-style application and nothing specific to any job.

### 3.1 What the core provides

- Layout: panels, splits, groups, maximizing, pop-out windows and workspaces.
- A drawing surface for every panel, and putting it on screen. The core offers **no drawing commands**: nothing like "draw a rectangle" or "draw text".
- Its own small interface: tab rows, dividers, drag highlights and menus.
- Input: focus, keybindings, the mouse pointer and text input.
- Events and actions for everything about panels and layout.
- Plugin loading, and the services and events through which plugins work together.
- Presets, sessions and settings.
- Operating-system features, such as popups, the clipboard, drag and drop, and dialogs (sections 5.6 and 11).
- Lua can access everything above.

### 3.2 What plugins provide

Everything else. If the core's own machinery needs it, it is core; if only some panels need it, it is a plugin. For example:

- Drawing shapes, text and images, and user-interface elements such as buttons and lists.
- Loading images, 3D models, audio and other media.
- Text editing, painting and all other tool logic.
- Audio, networking and 3D rendering.
- Even built-in panels, like settings, are written as plugins.

### 3.3 First-party plugins

Some plugins are made and shipped together with OpenECS because the product needs them:

- **sdl:** built into the executable. Gives native plugins the core's SDL objects (section 4).
- **ui:** a **standard plugin**: drawing (shapes, text, images) and user-interface elements, offered as a service to other plugins.
- **settings:** the settings window, built on ui.
- **launcher:** picks a preset or session when OpenECS starts without one, built on ui.

Apart from sdl, they are plugins like any other. They use only the plugin API, the core neither knows nor favours them, and a user can replace any of them with a plugin of the same name.

### 3.4 Platform

OpenECS targets Linux only for now.

### 3.5 Not priorities, and not in the design

- **Security.** There is no permission system and no protection against malicious plugins or files.
- **Accessibility.** The application is not designed with accessibility in mind.
- **Look and feel.** A theme is a few core settings (colours, font, sizes) that the core's interface uses and plugins can read.
- **Undo and redo.** Each panel or plugin keeps its own history and binds its own keys; the core keeps no global history.
- **Not in the design:** automatic saving while OpenECS runs, crash recovery, enabling or disabling plugins while OpenECS runs, and a library version of OpenECS.

## 4. Architecture

```
+--------------------------------------------------------------+
|  Plugins: native (C) and Lua                                 |
+--------------------------------------------------------------+
|  Plugin API: the only way plugins talk to the core           |
+--------------------------------------------------------------+
|  Core logic: layout, workspaces, input, events, services,    |
|  settings, presets and sessions, plugin loading              |
+--------------------------------------------------------------+
|  Libraries: SDL3, SDL3_ttf, Clay, Lua, libffi                |
+--------------------------------------------------------------+
```

**Plugins talk only to the plugin API.** They never receive an SDL, Lua, Clay or libffi object from it. For example, a native plugin that needs one of the core's SDL objects, such as the graphics device, gets it through the built-in **sdl** plugin.

## 5. Panels

### 5.1 What a panel is

A panel is a box in the layout. The core decides where the box is, how big it is, and whether it is visible or focused. The panel's type decides everything inside the box.

### 5.2 Drawing

Every panel gets its own **surface**, which the core owns and puts on screen. The panel draws into its surface, and the core assembles all surfaces into what the user sees. So one panel cannot paint over another, and the core can move a panel to another OS window without its help.

A panel type chooses one of two kinds of surface:

- **Pixels:** memory that the panel fills with the processor. The simplest kind, for image viewers, emulators or anything that computes its own pixels.
- **GPU:** an image on the graphics card that the panel draws into with the graphics card. For 3D views and fast interfaces.

Surfaces are measured in real screen pixels. Every panel knows its **scale**, which can be fractional. A panel is told when its scale changes, for example when it moves to another monitor.

### 5.3 When panels run

OpenECS does nothing unless something happens. Code runs because of:

- **input:** key presses and the pointer;
- **timers** that a plugin asks for, such as "every 100 ms" or "once, in 2 seconds";
- **continuous drawing:** a panel type can ask to be drawn every frame while it is visible, for animation or games;
- **background work** that finished on another thread.

Any other panel is drawn only when its content changed or its size changed. Panels in hidden workspaces are not drawn, but their timers keep running. An idle OpenECS uses no processor time.

### 5.4 State

Each panel has its own state. A panel type can save that state into the session and load it back. It records a version number for the state, so it can read older saves.

### 5.5 Unsaved work

A panel can mark itself as having **unsaved work** and can offer a save function. Plugins never block closing. Instead, before a panel with unsaved work closes, the core asks the user: Save, Discard or Cancel. The core asks whether the panel closes by itself, because the user quits, or because another session is being loaded.

### 5.6 Beyond the panel's box

Some things only the core can do, because they cross panel borders or belong to the OS window:

- **Popups.** A panel can open a popup, such as a menu, a list of suggestions or a tooltip. A popup may extend past the panel and even past the OS window.
- **Pointer.** A panel can set the pointer's shape while the pointer is over it, for example a text cursor or a crosshair. It can also lock the pointer, for example to turn a 3D camera. The lock ends when the panel loses focus.
- **Drag and drop** of data between panels (section 9.3).

## 6. Layout and workspaces

### 6.1 The layout tree

Every OS window holds one **layout tree** with three kinds of node:

- **Split:** divides an area horizontally or vertically. Each part has either a **fixed size**, like a toolbar or a status bar that keeps its height, or a **share** of the remaining space, so resizing keeps proportions.
- **Group:** one or more panels in the same place, one shown at a time.
- **Panel:** an instance of a panel type.

A panel type can declare a **minimum size**. Splits never make a panel smaller than that.

### 6.2 Tab rows

- A group with two or more panels shows a **tab row**. The user drags a tab to move its panel and right-clicks it for the panel's menu; a right click on the rest of the tab row opens the group's menu. A middle click on a tab closes its panel. Dragging the empty part of the tab row moves the whole group.
- A group with one panel shows no tab row. Instead, when the pointer comes near the top edge of the panel, the core shows a small **grip** with the panel's title at its top centre. Dragging the grip moves the panel; clicking or right-clicking it opens the panel's menu. A locked panel's grip says so, and only opens the menu.
- Dropping a panel onto the centre of another panel groups them, and the tab row appears. When a group is left with one panel, the tab row disappears.
- Tabs can be reordered by dragging, and a long tab row scrolls with the mouse wheel. A tab that becomes shown scrolls into view. A tab shows the panel's title, a mark for unsaved work and a close button.

### 6.3 Operations are plain functions

Every layout change is a function: open, split, move, group, close, pop out, swap, resize a divider, move focus, maximize, switch workspace. The mouse only works out the arguments and calls the same function that a script would call. A menu entry, like a right click menu, and a keybinding can always offer the same moves as dragging.

### 6.4 Dragging panels

While a panel or a group is dragged, the core highlights where it will land. A place where the drop would change nothing, such as the panel's own place, is not highlighted.

| Pointer position              | Result                      |
| ----------------------------- | --------------------------- |
| Centre of a panel             | Group with that panel.      |
| Near an edge of a panel       | Split in that direction.    |
| Over a tab row                | Insert between tabs.        |
| Near an edge of the OS window | Dock along that whole edge. |
| Outside every OS window       | Move into a new OS window.  |

On Wayland, the display system decides where a new OS window appears.

### 6.5 Panels opened by code

When a plugin opens a panel, for example when a file browser asks the text plugin to open a file, the panel goes to a predictable place. The caller can name a different place. By default, the panel joins the group of the most recently focused panel of the same type; if there is none, it joins the focused group.

### 6.6 Maximize, pop-out and workspaces

- **Maximize** makes one group fill its OS window. The other panels stay alive but hidden.
- **Pop-out** moves a panel into a new OS window. That window holds a full layout of its own. Floating panels that overlap others inside one OS window are not part of the design.
- Switching **workspaces** is instant, because nothing is rebuilt. Pop-out windows belong to their workspace and are hidden while the user is in another one.

### 6.7 Locked panels

A preset or a user can lock parts of the layout, for example a toolbar or a 3D view that must always be there. The user cannot move or close a locked panel unless they unlock it.

### 6.8 Menus

A panel's menu, opened from its tab or grip, has the core's entries (close, maximize, pop out, split, move to workspace) plus entries that the panel's type adds for its own panels. A group's menu, opened from the rest of its tab row, lists its tabs and acts on the whole group. Menus offer only what can be done now, and group entries of one kind into submenus. Inside its own area, a panel shows its own menus with popups.

## 7. Keys and focus

### 7.1 Focus

Exactly one panel has keyboard focus. How focus follows the pointer is a setting:

- **click:** clicking a panel focuses it;
- **hover:** the panel under the pointer gets focus.

Keys can also move focus to a neighbouring panel.

Pointer events always go to the panel under the pointer, whatever has focus. During a drag, they keep going to the panel where the drag started.

The default is *click*. In hover mode, only real pointer movement changes focus, not a layout change under a still pointer.

### 7.2 Keybindings are settings

A keybinding is a setting whose value is a key combination. It follows the same layers as every other setting (section 10.4), so the user can change any binding.

### 7.3 Who may bind what

- **Plugins** bind keys only for their own panel types. Those bindings work only while such a panel has focus.
- **Presets** and the **user's settings** can also bind keys for a workspace or for the whole tool, to any registered service function, by name. For example, a paint preset binds Ctrl+N to `canvas.new`, so Ctrl+N creates a canvas even while the palette has focus.

When several bindings match a key press, the binding set in the highest settings layer wins: the user's over the preset's over a plugin's. Within one layer, the most specific binding wins: one panel, then a panel type, then a workspace, then the whole tool. If no binding matches, the key goes to the focused panel, so a panel can run its own key logic, such as modal editing.

A key press that triggers a binding types no text.

### 7.4 The core's keys

The core reserves one key combination, the **core prefix**. After it, one more key chooses a core action: arrows move focus, Shift+arrows move the panel, numbers switch workspace, and so on. While the core waits for that key, it shows the available keys; Escape cancels.

So the core takes only one combination away from plugins. tmux's prefix key, Vim's Ctrl+W window commands and VS Code's Ctrl+K chords work the same way. The prefix is checked when the key is pressed, so it always wins, even if it is changed while OpenECS runs. The prefix is a setting, and the user can also bind core actions to direct keys.

The default prefix is **Alt+W**.

### 7.5 Keyboard layouts

Bindings follow what a key means on the user's layout, not where it sits: Ctrl+Z means "Ctrl and the key that types z". On layouts with non-Latin letters, such as Russian, letter keys count as their English letters, so shortcuts still work.

### 7.6 Sequences

Sequences of keys pressed one after another are up to panels. They can define how they will handle sequences like keybindings.

## 8. Plugins

### 8.1 Kinds

- **Native plugin:** compiled C code in a shared library. Fastest, and it must follow written rules about how it is built.
- **Lua plugin:** a script.

The two kinds are interchangeable: a service written in Lua can be called from C and the other way round, without hand-written glue code.

### 8.2 Where plugins live

A plugin is a folder that holds its manifest and its code. OpenECS looks for plugins in a few directories: one that the preset may name, the user's plugin directory, and the first-party plugins shipped with OpenECS.

### 8.3 Loading

At start-up, OpenECS reads the plugin manifests, works out the order from the dependencies, and loads the plugins the preset needs, together with the plugins they depend on. Missing and circular dependencies are reported clearly. Plugin authors never manage the order themselves. Each plugin registers what it provides when it starts. Plugins are not enabled or disabled while OpenECS runs.

### 8.4 What a plugin may do

A plugin's power depends on what it touches:

- **Input:** a plugin receives input and binds keys only for its own panels.
- **Panel state:** only a panel's own plugin can see or change it.
- **Layout:** any plugin may open, resize, move, focus or close any panel. The arrangement belongs to the user, not to one plugin, and closing still asks about unsaved work. Any plugin may also observe layout events.
- **Names:** everything a plugin registers starts with the plugin's name, such as `canvas.new` or `canvas.grid`, so names cannot collide.

This is not a security boundary. Its purpose is to stop plugins from getting in each other's way by accident. Native plugins run with full access to the program and its memory.

## 9. Plugin communication

### 9.1 Services

A plugin offers functions to others by registering a **service**. Each function is registered with a **signature**. OpenECS checks the signature when the function is registered, and uses it to call the function from Lua or C alike.

### 9.2 Panels are views; data lives in services

A panel shows data, but it does not have to own it. Data that several panels share, such as a document, a scene or the current selection, lives in a plugin's service. Then two panels can show the same file, the same canvas can appear in two workspaces, and an inspector can show what is selected in a 3D view.

To make this work:

- **Plugin events.** A plugin can declare its own events, such as `canvas.selectionChanged`, and send them. Other plugins subscribe to them.
- **Plugin state.** A plugin, not only a panel, can save state into the session. When a session is loaded, plugin state is restored before panels, so panels find their data.

### 9.3 Drag and drop of data

A panel can start dragging data, labelled with a type such as `file-list` or `color`. Panels say which types they accept. The core highlights the panels that accept the dragged type and delivers the data to the one it is dropped on. Files and text dropped from other applications arrive the same way.

## 10. Presets, sessions and settings

### 10.1 Presets

A **preset** is a hand-written Lua file that describes a tool:

- its identity: a name, an icon and an application id, so that each tool appears to the desktop as its own application, with its own entry in the application menu and its own window rules;
- the plugins it needs;
- settings and keybindings;
- workspaces and panels.

A preset is a complete application representation. That is why a preset may do things that plugins may not, such as binding keys for the whole tool. A preset can use logic, such as loops and conditions, to build its contents, but it cannot call the core or plugins while it is read.

### 10.2 Sessions

A **session** uses the same format as a preset, but OpenECS writes it.

- When the user quits, OpenECS saves the session and restores it the next time the same tool starts. This is not automatic saving: nothing is saved while OpenECS runs.
- The user can save a session to a file at any time and open it later.
- A session is a snapshot. It does not link back to the preset it started from.

### 10.3 Applying a session

- **Nothing that cannot be understood is destroyed.** If a panel's type is missing, a placeholder keeps the panel's saved state, so saving again writes it back. One bad panel does not stop the rest.
- **Every panel type and plugin owns its state format,** with a version, and is told which version it is reading.

### 10.4 Settings

Settings come in layers. A higher layer overrides the lower ones:

1. The core's settings, from a file shipped with OpenECS.
2. Plugin defaults.
3. The preset.
4. Changes made in the settings window, kept in a file that OpenECS writes.
5. The user's hand-edited settings file.

The user's hand-edited file is the top layer, and the core never rewrites it, so the user's comments and formatting stay. The settings window shows a setting that a higher layer overrides as faded, with a hint that names the file where it is set.

- Settings have types, so a wrong value in a hand-edited file produces a clear message.
- Settings of plugins that are not loaded are kept, not removed.
- The user's files can hold settings for every tool and, in a separate section, settings for one tool only.
- The user's settings can name extra plugins to load in every tool, for example the user's own Lua scripts.

## 11. Operating-system integration

- The core provides the clipboard (text and other typed data), drag and drop with other applications, file open and save dialogs, and message dialogs.
- Plugins provide a system tray icon and desktop notifications.

## 12. Principles

1. **Shallow core.** The core provides mechanisms. Plugins and settings decide what is shown and what keys do. Nothing domain-specific goes into the core.
2. **Everything is available from Lua.** Lua can do everything that C can.
3. **Plain functions.** Actions are plain functions that code calls directly. Settings and presets refer to them by their service name. There is no separate command system.
4. **One front door.** Plugins reach the core only through the plugin API. The core checks its rules when something is used, not only when it is registered.
5. **Small, uniform API.** Few concepts, used the same way everywhere.
6. **Hide dependencies.** Plugins never get SDL, Lua, Clay or libffi from the API, so these can change without breaking plugins.
7. **Least authority.** A plugin can do only what its job needs (section 8.4). Presets and the user can do more, because they assemble the tool.
8. **The user decides.** The user's settings and keybindings win, the core's keys are always reachable, and no plugin can block closing.
9. **Panels are views.** Shared data lives in services, not inside panels.
10. **Nothing runs without a reason.** Code runs because of input, timers, visible animation or finished background work, never by polling.
11. **Do not lose what is not understood.** Unknown settings, missing panel types and unreadable state are kept.
12. **Errors stay local.** A broken script or panel affects only itself.
13. **The core's own tools are plugins.** Rich windows, such as the settings window, are built on the plugin API, which proves that the API is complete.

## Glossary

**Action.** A function that makes something happen, such as closing a panel.

**Clay.** A layout library. It computes the sizes and positions of rectangles.

**Continuous drawing.** A panel type's request to be drawn every frame while it is visible.

**Core.** OpenECS without plugins.

**Core prefix.** The one key combination reserved for the core. The key pressed after it chooses a core action.

**Dependency.** Something that a piece of software needs in order to work.

**Docking.** Attaching a panel to a place in the layout, by splitting or grouping.

**Event.** A notification that something happened.

**Executable.** A program that can be run directly.

**First-party plugin.** A plugin made and shipped together with OpenECS.

**Focus.** The one panel that receives keyboard input.

**GPU.** The graphics card.

**Grip.** A small handle that the core shows on a panel without a tab row, used to move the panel or open its menu.

**Group.** One or more panels that share one place in the layout; one of them is shown at a time.

**Keybinding.** A link between a key combination and a function. It is a setting.

**Launcher.** The tool that OpenECS shows when it starts without a preset. It lists presets and saved sessions, and opens the one the user chooses.

**Layout tree.** The splits, groups and panels inside one OS window.

**Least authority.** The rule that a plugin can do only what its job needs.

**libffi.** A library that calls a C function when only its signature is known at run time.

**Lua.** A small scripting language, used for plugins and for presets, sessions, settings and manifests.

**Manifest.** A small file next to a plugin that gives its name, version and dependencies.

**Modal editing.** Editing in which the same key does different things depending on the current mode.

**Native plugin.** A plugin made of compiled C code in a shared library.

**OS window.** A window of the operating system, with its own title bar. It holds one layout tree.

**Panel.** A rectangular area of the layout whose contents come from a plugin.

**Panel type.** What a kind of panel shows and does. Many panels can share one type.

**Placeholder.** A stand-in panel that the core shows when a panel's type is missing or has failed. It keeps the panel's saved state.

**Plugin.** An add-on module that provides panel types, services, events or settings.

**Plugin API.** The functions and types through which plugins use the core.

**Standard plugin.** A first-party plugin whose services other plugins build on, such as ui.

**Pop-out.** Moving a panel into a new OS window.

**Popup.** A small, temporary window, such as a menu or a tooltip, that belongs to a panel.

**Preset.** A hand-written file that describes a tool.

**Register.** To tell OpenECS, by calling one of its functions, that something exists.

**Scale.** How many screen pixels make one unit of the layout.

**SDL.** A library for OS windows, input, graphics and other system features.

**SDL3_ttf.** SDL's library for drawing text.

**Service.** A named set of functions that a plugin offers to other plugins and scripts.

**Session.** The saved state of OpenECS: plugins, workspaces, panels and their state.

**Setting.** A named, typed value that configures the core or a plugin.

**Shared library.** A compiled code file that a program can load while it runs (`.so` on Linux).

**Signature.** A short description of what a function takes and returns.

**Split.** A layout node that divides an area horizontally or vertically.

**Surface.** The picture a panel draws into.

**Tab row.** The row of tabs that a group with two or more panels shows.

**Timer.** A request to run a function after some time, once or repeatedly.

**Unsaved work.** A panel's mark that it has changes that are not saved yet.

**Wayland.** A display system used on Linux.

**Workspace.** A named arrangement of panels and OS windows that can be switched to instantly.
