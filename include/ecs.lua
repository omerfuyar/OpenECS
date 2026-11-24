---@meta ecs

-- The ecs module of OpenECS for Lua plugins: local ecs = require("ecs").
-- This file only describes the module for editors; OpenECS gives each plugin its own ecs table.

-- A function that keys can run says so. Keys call it with no arguments, which acts on the focused panel as the user does: locks stop it.
-- A function that creates a name, such as ecs.panel.registerType, takes the local name: the core adds the plugin's name and a dot, so "view" of the plugin canvas is "canvas.view".
-- Every other function takes full names.

---A colour: "#RRGGBB", "#RRGGBBAA" with opacity, or the name of a colour of the theme, such as "text" for the setting ecs.colorText.
---The names are the core's colour settings, ecs.color and the name with a capital first letter.
---@alias ecs.Color
---| "background" # ecs.colorBackground: the title bar, and between panels
---| "tabRow" # ecs.colorTabRow: tab rows, and headers inside panels
---| "tab" # ecs.colorTab: tabs
---| "tabShown" # ecs.colorTabShown: the shown tab
---| "text" # ecs.colorText: text
---| "textDim" # ecs.colorTextDim: dim text
---| "accent" # ecs.colorAccent: the focus, links, keys, headings and what is on
---| "placeholder" # ecs.colorPlaceholder: a placeholder panel
---| "overlay" # ecs.colorOverlay: menus, grips, tooltips and the list of prefix keys
---| "drop" # ecs.colorDrop: the place where a dragged panel lands
---| "selected" # ecs.colorSelected: the chosen item, menu entry or text
---| "frame" # ecs.colorFrame: the line around each group, menu and field
---| "panel" # ecs.colorPanel: what a panel's view is drawn on
---| "field" # ecs.colorField: fields, check boxes, and the tracks of sliders and scroll bars
---| "button" # ecs.colorButton: buttons and the other controls that are clicked
---| "buttonHover" # ecs.colorButtonHover: a button under the pointer
---| "hover" # ecs.colorHover: a row or menu entry under the pointer
---| string

---@class ecs
local ecs = {}

-- Log

---@class ecs.log
ecs.log = {}

---Writes a debug message in the plugin's name.
---@param text string The message, one line or more.
function ecs.log.debug(text) end

---Writes an information message in the plugin's name.
---@param text string The message, one line or more.
function ecs.log.info(text) end

---Writes a warning in the plugin's name.
---@param text string The message, one line or more.
function ecs.log.warn(text) end

---Writes an error in the plugin's name.
---@param text string The message, one line or more.
function ecs.log.error(text) end

-- Errors

---Reports a failure that the plugin's function returns: logs it as a warning while ecs.logServiceErrors is on, and gives the message.
---A function fails this way when something outside its caller goes wrong, such as a missing file: return nil, ecs.fail("no file " .. path).
---@param message string What failed.
---@return string message The message.
function ecs.fail(message) end

---Checks that the plugin's function is used by its rules. When the condition is false or nil, the call is a misuse:
---the core logs it in the name of the plugin whose callback made it and stops the Lua code. A misuse in a panel's callback faults the panel; in any other, the plugin is disabled.
---@param condition any The rule, such as type(path) == "string".
---@param message string What the caller broke.
function ecs.check(condition, message) end

-- Settings

---@alias ecs.SettingType "bool"|"integer"|"number"|"string"|"choice"|"key"|"list"|"table"|"color"|"path"

---@class ecs.SettingDesc
---@field name string The local name, such as "grid"; the setting is "canvas.grid".
---@field type ecs.SettingType
---@field description? string
---@field choices? string[] The texts a "choice" setting can have.
---@field default? any
---@field changed? fun(value: any) Called with the new value when the setting changes.

---@class ecs.setting
ecs.setting = {}

---Declares a setting of the plugin.
---@param desc ecs.SettingDesc The setting's local name, type, default and description.
---A declaration that is not valid, such as a second one of a name, is a misuse (ecs.check).
function ecs.setting.declare(desc) end

---Gets a setting's value from the highest layer that sets it.
---@param name string The setting's full name, such as "canvas.grid".
---@return any value Its value in effect, or nil if no setting has the name.
function ecs.setting.get(name) end

---Sets a setting in the user's settings file, the highest layer, and writes the file.
---A file that could not be read is not written: the change lasts until the program exits.
---@param name string The setting's full name, such as "canvas.grid".
---@param value any The new value, of the setting's type; nil takes the user's value away, so the value of the layers below applies.
---@return true|nil ok true if the value is set and written.
---@return string? message Why the setting is not set.
function ecs.setting.set(name, value) end

---Sets a setting for this run only, in a layer above the user's settings file that no file keeps, such as a colour while a tool is busy.
---@param name string The setting's full name, such as "ecs.colorFrame".
---@param value any The new value, of the setting's type; nil takes the run's value away, so the value of the layers below applies.
---@return true|nil ok true if the value is set.
---@return string? message Why the setting is not set.
function ecs.setting.setForRun(name, value) end

---Lists the names of every declared setting, in the order they were declared.
---@return string[]? names The full names, such as "canvas.grid".
---@return string? message Why the names cannot be listed.
function ecs.setting.list() end

---A declared setting, as ecs.setting.explain gives it: its description, with its full name, and where its value comes from.
---@class ecs.SettingExplanation: ecs.SettingDesc
---@field name string The full name, such as "canvas.grid".
---@field owner string The plugin that declared the setting.
---@field value any The value in effect.
---@field layer "default"|"theme"|"preset"|"user"|"run" The layer the value comes from.
---@field file? string The file of that layer; missing for defaults.
---@field layers { default: any, theme: any, preset: any, user: any, run: any } The value of each layer that sets it.

---Explains a setting: its value in effect, the layer it comes from, and what each layer says.
---@param name string The setting's full name, such as "canvas.grid".
---@return ecs.SettingExplanation? explanation The setting, its value and its layers; nil if it is not declared.
---@return string? message Why it cannot be explained.
function ecs.setting.explain(name) end

-- Timers

---@class ecs.Timer
local Timer = {}

---Stops the timer. A timer that already ended does nothing.
function Timer:stop() end

---@class ecs.timer
ecs.timer = {}

---Starts a timer of the plugin. A panel's timer is stopped in its type's destroy, as its subscriptions end there.
---@param seconds number More than 0, or 0 for a one-shot timer.
---@param repeats boolean true: it runs every seconds until stopped; false: it runs once.
---@param callback fun() Runs on the main thread when the time comes.
---@return ecs.Timer timer The timer, which stops it.
function ecs.timer.start(seconds, repeats, callback) end

-- Workers: Lua scripts on threads of their own (DESIGN 3.4)

---A worker that ecs.worker.start started.
---@class ecs.Worker
---@field running boolean Whether its script still runs: it has not ended, and was not stopped.
local Worker = {}

---Sends a copy of a value to the worker, which its ecs.worker.receive gives. A worker that ended or was stopped gets nothing.
---@param value any Nil, a boolean, a number, a text, or a table of them, which string.buffer encodes; a function, a handle or a struct is an error.
function Worker:send(value) end

---Stops the worker: its script ends at its next receive, or within a few thousand instructions. Its message and done functions are not called again.
function Worker:stop() end

---@class ecs.WorkerDesc
---@field file string The script, a path in the plugin's folder.
---@field input? any A value that the script gets as its `...`, copied as messages are.
---@field message? fun(value: any) Gets each value that the script sends, on the main thread.
---@field done? fun(ok: boolean, result: any) Gets the end of the script: true and what it returned, or false and its error.

---@class ecs.worker
ecs.worker = {}

---Starts a worker: a Lua script that runs on a thread of its own, in a Lua state of its own. Lua only: a native plugin runs work with ECS_RunInBackground, or on threads of its own.
---@param desc ecs.WorkerDesc The script, its input, and the functions that get its messages and its end.
---@return ecs.Worker? worker The worker, which sends to it and stops it; nil if it did not start.
---@return string? message Why it did not start.
function ecs.worker.start(desc) end

---In a worker's script only: gives the oldest value that the plugin sent, waiting for one at most seconds, or without end. Gives nil when the time runs out.
---@param seconds? number How long to wait at most; without end if missing.
---@return any value The value, or nil if the time ran out.
function ecs.worker.receive(seconds) end

---In a worker's script only: sends a copy of a value to the plugin, whose message function gets it.
---@param value any Nil, a boolean, a number, a text, or a table of them, which string.buffer encodes; a function, a handle or a struct is an error.
function ecs.worker.send(value) end

-- Panels

---Where a panel is in its OS window, in its surface pixels: panel.place.
---@class ecs.PanelPlace
---@field x number The panel's left edge, from the window's.
---@field y number The panel's top edge, from the window's.
---@field windowWidth number The window's width.
---@field windowHeight number The window's height.

---A panel: a handle of type ecs.panel. Its data are fields, and what it does are methods.
---@class ecs.Panel
---@field title string The title its tab shows; setting it changes the tab.
---@field id integer Read only: the panel's id, which stays the same across sessions, and which events and ecs.layout.findPanel use.
---@field type string Read only: the name of the panel's type, such as "canvas.view".
---@field unsaved boolean Whether the panel has unsaved work, which shows a mark on its tab; before it closes, the user is asked to save it.
---@field place ecs.PanelPlace Read only: where the panel is in its OS window, and the window's size, in its surface pixels, so a popup's anchor can be anywhere in the window; zeros while it is not shown.
local Panel = {}

---Asks for the panel to be drawn again.
function Panel:redraw() end

---Says whether the panel accepts typed text, and where its text cursor is. While the panel has focus and accepts text, it gets text events,
---and the system's input method shows its window beside the cursor. A key that runs a binding types no text.
---@param accept boolean true: the panel accepts typed text while it has focus; false: it does not.
---@param x? number The text cursor's rectangle, in surface pixels; 0 if missing.
---@param y? number The text cursor's top, in surface pixels; 0 if missing.
---@param width? number The text cursor's width, in surface pixels; 0 if missing.
---@param height? number The text cursor's height, in surface pixels; 0 if missing.
function Panel:setTextInput(accept, x, y, width, height) end

---Sets the types of data that the panel accepts when data is dropped on it, such as "color" or "file-list".
---Data from other applications is "file-list", a list of paths, or "text". A new call replaces the list.
---@param types string[] The data types the panel accepts; an empty list accepts none.
function Panel:acceptDrops(types) end

---Starts dragging data from the panel, while a pointer button that was pressed on it is held, usually from its pointerDown or pointerMove event.
---The panel then gets no pointer moves until the release, and still gets its pointerUp. On release, the panel under the pointer that accepts the type gets a drop event. Escape cancels.
---@param type string The type of the data, such as "color".
---@param value? any The data: a value as in saved state.
---@return true|nil ok true if the drag started.
---@return string? message Why no drag starts: no pointer button that was pressed on the panel is held.
function Panel:startDrag(type, value) end

---Opens a popup next to a rectangle of the shown panel: below it, or above it if there is no room below.
---It closes when code closes it, on a press outside every popup, on Escape if it is a menu, and when the panel closes, fails or is hidden.
---@param desc ecs.PopupDesc The popup's type, place, size and functions.
---A desc without draw or a size above 0 is a misuse (ecs.check).
---@return ecs.Popup? popup The popup, which closes, draws again and resizes it; nil if the panel is not shown.
---@return string? message Why the popup is not opened.
function Panel:openPopup(desc) end

---The picture a panel draws into: a handle of type ecs.surface, which a panel can pass to a service that draws. It is valid only while draw runs.
---A GPU surface is a texture that the gpu standard plugin draws into, with gpu.frame(surface); it has no pixels, so its methods raise an error.
---@class ecs.Surface
---@field width integer
---@field height integer
---@field scale number Pixels per layout unit.
---@field type "pixels"|"gpu" The panel type's surface.
---@field kept boolean The pixels still hold the picture of the last draw, so draw may change only what changed. False for a new surface, such as after a resize, and for a GPU surface.
local Surface = {}

---Tells the core which rectangle the draw changed, so it shows only that; the core takes the rectangle around every one given.
---A draw that tells none changed the whole surface; one that tells an empty rectangle changed nothing.
---@param x integer The rectangle's left, in pixels.
---@param y integer The rectangle's top, in pixels.
---@param width integer The rectangle's width, in pixels.
---@param height integer The rectangle's height, in pixels.
function Surface:changed(x, y, width, height) end

---Fills a rectangle with one colour, such as to clear the surface. The rectangle is in pixels, and clipped to the surface.
---@param x integer The rectangle's left, in pixels.
---@param y integer The rectangle's top, in pixels.
---@param width integer The rectangle's width, in pixels.
---@param height integer The rectangle's height, in pixels.
---@param color ecs.Color It replaces the pixels.
function Surface:fill(x, y, width, height, color) end

---Sets a pixel. Pixels outside the surface are clipped. Lua only: C writes the surface's pixels directly.
---@param x integer The pixel's column.
---@param y integer The pixel's row.
---@param color ecs.Color The colour it takes, alpha and all.
function Surface:setPixel(x, y, color) end

---Lua only: C reads the surface's pixels directly.
---@param x integer The pixel's column.
---@param y integer The pixel's row.
---@return string color "#RRGGBBAA".
function Surface:getPixel(x, y) end

---Copies a row of pixels, clipped to the surface. Lua only: C writes the surface's pixels directly.
---@param y integer The row.
---@param bytes string|string.buffer|ffi.cdata* ARGB pixels, 4 bytes each in native byte order.
---@param x? integer The first pixel's column; 0 if missing.
function Surface:setRow(y, bytes, x) end

---@alias ecs.PanelEventType "pointerDown"|"pointerUp"|"pointerMove"|"wheel"|"keyDown"|"keyUp"|"focused"|"unfocused"|"shown"|"hidden"|"resized"|"drop"|"text"

---@class ecs.PopupDesc
---@field type? "menu"|"tooltip" A menu takes the key presses while it is open, and Escape closes it; a tooltip takes none. "menu" if missing.
---@field anchor? { x: number, y: number, width: number, height: number } The rectangle of the panel it opens next to, in the panel's surface pixels.
---@field width number In layout units.
---@field height number In layout units.
---@field draw fun(surface: ecs.Surface) Draws the popup; what it leaves out is transparent.
---@field event? fun(event: ecs.PanelEvent) Gets pointer, wheel and key events, with positions in the popup's surface pixels.
---@field closed? fun() Says the popup has closed; it is invalid afterwards.

---A popup of a panel: a handle of type ecs.popup.
---@class ecs.Popup
local Popup = {}

---Closes the popup. Its closed function runs after the current callback returns.
function Popup:close() end

---Asks for the popup to be drawn again.
function Popup:redraw() end

---Changes the popup's size, in layout units, and draws it again.
---@param width number The new width, in layout units.
---@param height number The new height, in layout units.
function Popup:setSize(width, height) end

---@class ecs.PanelEvent
---@field type ecs.PanelEventType
---@field shift boolean
---@field ctrl boolean
---@field alt boolean
---@field super boolean
---@field x? number For pointer, wheel and drop events.
---@field y? number For pointer, wheel and drop events.
---@field button? integer For pointerDown and pointerUp.
---@field scrollX? number For wheel events.
---@field scrollY? number For wheel events.
---@field key? string SDL's key name, for keyDown and keyUp.
---@field width? number For shown and resized.
---@field height? number For shown and resized.
---@field dataType? string For drop: the type of the dropped data, such as "color" or "file-list".
---@field value? any For drop: the dropped data.
---@field text? string For text: the typed text, UTF-8.

---@class ecs.PanelTypeDesc
---@field name string The local name, such as "view"; the type is "canvas.view".
---@field title? string The default title of tabs and menus.
---@field surface? "pixels"|"gpu" "pixels" if missing; "gpu" gives draw a GPU texture, which the gpu standard plugin draws into.
---@field stateVersion? integer The version of the saved state.
---@field continuous? boolean true: draw runs every frame while the panel is shown, for animation. false, or missing: draw runs only when the panel is shown, resized or asks with panel:redraw().
---@field minWidth? number In layout units.
---@field minHeight? number In layout units.
---@field create? fun(panel: ecs.Panel, saved: any, version: integer): any Gives the panel's state.
---@field destroy? fun(state: any)
---@field draw? fun(state: any, surface: ecs.Surface, seconds: number) Seconds since the panel was last drawn.
---@field event? fun(state: any, event: ecs.PanelEvent)
---@field saveState? fun(state: any): any Gives the state to save in the session.
---@field save? fun(state: any): true|nil, string? Saves unsaved work: true, or nil and why not.

---@class ecs.panel
ecs.panel = {}

---Registers a panel type.
---@param desc ecs.PanelTypeDesc The type's local name, title, surface and functions.
---A registration that is not valid, such as a second one of a name, is a misuse (ecs.check).
function ecs.panel.registerType(desc) end

---Adds a service function to the panel menu of a type. Its signature is "void(handle<ecs.panel>)" or "void()".
---@param type string The panel type, by its full name, such as "canvas.view".
---@param functionName string The service function the entry runs, such as "canvas.clear".
---A type that is not the plugin's, or a function that cannot be an entry, is a misuse (ecs.check).
function ecs.panel.addMenuEntry(type, functionName) end

---A registered panel type in a list.
---@class ecs.PanelTypeEntry
---@field name string The panel type's name, such as "canvas.view", which ecs.layout.open takes.
---@field title string Its default title.
---@field plugin string The name of the plugin that registered it.
---@field description string That plugin's description, from its manifest.

---Lists the registered panel types, so a plugin can offer to open them.
---@return ecs.PanelTypeEntry[] types Sorted by plugin, then by title.
function ecs.panel.listTypes() end

---Restarts the focused panel from its last saved state, if it failed. Keys can run it.
function ecs.panel.restart() end

-- Layout and workspaces

---"window" is a new pop-out window, which needs no target.
---@alias ecs.Zone "default"|"center"|"left"|"right"|"top"|"bottom"|"window"

---@class ecs.layout
---@field moveToWorkspace1 fun() Moves the focused panel into workspace 1's focused group. Keys can run it.
---@field moveToWorkspace2 fun() Moves the focused panel into workspace 2's focused group. Keys can run it.
---@field moveToWorkspace3 fun() Moves the focused panel into workspace 3's focused group. Keys can run it.
---@field moveToWorkspace4 fun() Moves the focused panel into workspace 4's focused group. Keys can run it.
---@field moveToWorkspace5 fun() Moves the focused panel into workspace 5's focused group. Keys can run it.
---@field moveToWorkspace6 fun() Moves the focused panel into workspace 6's focused group. Keys can run it.
---@field moveToWorkspace7 fun() Moves the focused panel into workspace 7's focused group. Keys can run it.
---@field moveToWorkspace8 fun() Moves the focused panel into workspace 8's focused group. Keys can run it.
---@field moveToWorkspace9 fun() Moves the focused panel into workspace 9's focused group. Keys can run it.
---@field moveToWorkspace10 fun() Moves the focused panel into workspace 10's focused group. Keys can run it.
---@field moveToNewWorkspace fun() Moves the focused panel into a new workspace, after the last one and named after the panel's title; the current workspace stays shown. A tool has at most 10 workspaces. Keys can run it.
ecs.layout = {}

---Finds a panel by its id.
---@param id integer The panel's id, as a panel's id field gives it.
---@return ecs.Panel? panel The panel, or nil if no open panel has the id.
function ecs.layout.findPanel(id) end

---Opens a panel. Not in a panel's or popup's draw.
---@param type string The panel type, such as "canvas.view".
---@param saved? any The saved state the panel starts from.
---@param target? ecs.Panel The panel to open it next to.
---@param zone? ecs.Zone Where, next to the target; "default" if missing.
---@return ecs.Panel? panel The new panel, or nil if it is not opened.
---@return string? message Why the panel is not opened.
function ecs.layout.open(type, saved, target, zone) end

---Moves a panel next to another one, also from another workspace or OS window; or out into a new pop-out window with the zone "window". A panel alone in its OS window stays. Not in a panel's or popup's draw.
---@param panel ecs.Panel The panel to move.
---@param target? ecs.Panel Needed, except with the zone "window".
---@param zone? ecs.Zone "center" if missing.
---@return true|nil ok true if the panel moved.
---@return string? message Why the panel did not move.
function ecs.layout.move(panel, target, zone) end

---Closes a panel. If it has unsaved work, the user is asked first and may cancel. Not in a panel's or popup's draw.
---Keys run ecs.layout.close as the user's close: it closes the focused panel, unless its group is locked.
---@param panel ecs.Panel The panel to close.
---@return boolean closed true if the panel closed; false if the user kept its unsaved work.
function ecs.layout.close(panel) end

---Focuses a panel, and shows its workspace, group and OS window.
---@param panel ecs.Panel The panel to focus.
function ecs.layout.focus(panel) end

---Gives the focused panel.
---@return ecs.Panel? panel The focused panel of the shown workspace, or nil if it has none.
function ecs.layout.getFocus() end

---Focuses the panel on the left of the focused one. Keys can run it.
function ecs.layout.focusLeft() end

---Focuses the panel on the right of the focused one. Keys can run it.
function ecs.layout.focusRight() end

---Focuses the panel above the focused one. Keys can run it.
function ecs.layout.focusUp() end

---Focuses the panel below the focused one. Keys can run it.
function ecs.layout.focusDown() end

---Moves the focused panel into the group on its left, or along that edge of the OS window if there is none. Keys can run it.
function ecs.layout.moveLeft() end

---Moves the focused panel into the group on its right, or along that edge of the OS window if there is none. Keys can run it.
function ecs.layout.moveRight() end

---Moves the focused panel into the group above it, or along that edge of the OS window if there is none. Keys can run it.
function ecs.layout.moveUp() end

---Moves the focused panel into the group below it, or along that edge of the OS window if there is none. Keys can run it.
function ecs.layout.moveDown() end

---Shows the next tab of the focused panel's group. Keys can run it.
function ecs.layout.nextTab() end

---Maximizes the focused panel's group, or restores it. Keys can run it.
function ecs.layout.maximize() end

---Pops the focused panel out into a new OS window, unless it is locked or alone in its OS window. Keys can run it.
function ecs.layout.popOut() end

---Closes the panels of the focused panel's group, unless it is locked. Keys can run it.
function ecs.layout.closeGroup() end

---Locks the focused panel's group, or unlocks it. Keys can run it.
function ecs.layout.lock() end

---Opens the last closed panel again. Keys can run it.
function ecs.layout.reopen() end

---Builds every workspace again from the preset, or from the session the command line named, after asking about unsaved work. The plugins keep running. Keys can run it.
function ecs.layout.reset() end

---@class ecs.workspace
---@field switch1 fun() Switches to workspace 1. Keys can run it.
---@field switch2 fun() Switches to workspace 2. Keys can run it.
---@field switch3 fun() Switches to workspace 3. Keys can run it.
---@field switch4 fun() Switches to workspace 4. Keys can run it.
---@field switch5 fun() Switches to workspace 5. Keys can run it.
---@field switch6 fun() Switches to workspace 6. Keys can run it.
---@field switch7 fun() Switches to workspace 7. Keys can run it.
---@field switch8 fun() Switches to workspace 8. Keys can run it.
---@field switch9 fun() Switches to workspace 9. Keys can run it.
---@field switch10 fun() Switches to workspace 10. Keys can run it.
---@field menu fun() Shows the menu of workspaces, by number and name; an entry switches to its workspace. Keys can run it.
ecs.workspace = {}

---Counts the workspaces.
---@return integer count How many workspaces there are.
function ecs.workspace.getCount() end

---Gives the workspace that shows.
---@return integer number The shown workspace, from 1.
function ecs.workspace.getCurrent() end

---Gives a workspace's name.
---@param number integer From 1.
---@return string? name The workspace's name, or nil if there is no such workspace.
function ecs.workspace.getName(number) end

---Shows a workspace. Keys run it as ecs.workspace.switch1 to switch10.
---@param number integer From 1.
function ecs.workspace.switch(number) end

-- App

---@class ecs.app
---@field menu fun() Opens the title bar's menus, the tool's menu: their first one, where Left and Right go to the others; or, when the title bar does not show them all, one menu with them as submenus near the focused panel. Keys can run it.
ecs.app = {}

---Adds one of the plugin's functions to the title bar's menu Tools. Its signature is "void(handle<ecs.panel>)" or "void()".
---@param functionName string The plugin's service function the entry runs, such as "settings.open".
---A function that is not the plugin's is a misuse (ecs.check).
function ecs.app.addMenuEntry(functionName) end

---Quits, as the main window's close button does: the user is asked about unsaved work first and may cancel. Keys can run it.
function ecs.app.quit() end

-- Plugins

---A plugin in the plugin folders, as ecs.plugins.list gives it.
---@class ecs.PluginInfo
---@field name string
---@field version string
---@field description string
---@field enabled boolean Its code runs.
---@field failed boolean Its code failed to start.
---@field depends string[] The names of the plugins it depends on.

---@class ecs.plugins
ecs.plugins = {}

---Lists every plugin in the plugin folders, loaded or not.
---@return ecs.PluginInfo[] plugins
function ecs.plugins.list() end

---Enables a plugin, and the plugins it depends on, when the current pass of the main loop ends. The core emits ecs.pluginsChanged after it.
---@param name string The plugin's name.
function ecs.plugins.enable(name) end

---Disables a plugin when the current pass of the main loop ends, after asking about the unsaved work of its panels, which become placeholders. A plugin that an enabled plugin depends on is not disabled, which is reported. The core emits ecs.pluginsChanged after it.
---@param name string The plugin's name.
function ecs.plugins.disable(name) end

---Reloads a plugin, and the plugins that depend on it, when the current pass of the main loop ends: disables them without asking, as their panels keep their state in placeholders, and enables them again from their files. A plugin that cannot be started again is reported, and starts when its files change while ecs.reloadPlugins is on.
---@param name string The plugin's name: an enabled plugin, or one that a reload failed to start.
function ecs.plugins.reload(name) end

-- Profile

---A pass of the main loop, as ecs.profile.get gives it: the seconds of each of its steps (DESIGN 3.1).
---@class ecs.ProfilePass
---@field wait number Waiting for input, a timer or a frame.
---@field input number Handling input.
---@field timers number Running timers.
---@field events number Delivering events, and the rest of the pass before the frame.
---@field layout number Updating the layout and the popups.
---@field panels number Panels drawing.
---@field upload number Copying the panels' pixels to their textures.
---@field interface number The core's own interface.
---@field present number Presenting the OS windows.

---A plugin's or a panel's time in the last whole second, as ecs.profile.get gives it.
---@class ecs.ProfileTime
---@field name string? A plugin's name; "ecs" is the core's own callbacks and services.
---@field id integer? A panel's id.
---@field type string? A panel's type.
---@field seconds number Its time in that second: a plugin's own, without the plugins it called; a panel's Draw with the plugins it called.
---@field longest number Its longest call, in seconds.
---@field calls integer

---A Lua function's time in the last whole second, as LuaJIT's sampling profiler counted it, as ecs.profile.get gives it.
---@class ecs.ProfileFunction
---@field name string Its file, from its plugin's folder, and its name or the line where it starts, such as "init.lua:element".
---@field plugin string The plugin whose folder holds the file; "ecs" for the core's own Lua code.
---@field samples integer The samples that found it running, one each millisecond.
---@field seconds number Its time in that second, from its samples.

---Where the main thread's time went, as ecs.profile.get gives it.
---@class ecs.Profile
---@field passes ecs.ProfilePass[] The last passes of the main loop, the oldest first.
---@field plugins ecs.ProfileTime[] The plugins whose code ran in the last whole second, the most time first.
---@field panels ecs.ProfileTime[] The panels that drew in the last whole second, the most time first.
---@field functions ecs.ProfileFunction[] The 50 Lua functions that took the most of the last whole second, the most first.

---@class ecs.profile
ecs.profile = {}

---Gives where the main thread's time went while the setting ecs.profile is on (DESIGN 3.5).
---@return ecs.Profile? profile nil while ecs.profile is off.
function ecs.profile.get() end

-- Session

---@class ecs.session
ecs.session = {}

---Writes the session to a file: the plugins' state, the workspaces and the panels with their saved state. Quitting still saves the tool's last session.
---Keys run ecs.session.save, which asks for the file with a save dialog.
---@param path string The file. Missing folders are created.
---@return true|nil ok true if the session is written.
---@return string? message Why the session is not saved.
function ecs.session.save(path) end

---Opens a session in place of the current one: it asks about unsaved work, and once the current pass of the main loop ends, OpenECS saves the tool's last session, stops and starts again from the session.
---Keys run ecs.session.open, which asks for the file with an open dialog.
---@param path string A session file. A preset is refused; ecs.session.openPreset opens it.
---@return true|nil ok true if the session will open once the current pass of the main loop ends.
---@return string? message Why the session is not opened: the file is not a session, the user keeps the unsaved work, or a test runs.
function ecs.session.open(path) end

---Opens a preset's tool in place of the current one, as ecs.session.open opens a session. The tool starts from its last session if it has one.
---@param nameOrPath string A preset's name, looked up in the user's presets, then in the first-party presets; or a path, which has a '/' or ends with .lua.
---@return true|nil ok true if the tool will open once the current pass of the main loop ends.
---@return string? message Why the preset is not opened: the file is not a preset, the user keeps the unsaved work, or a test runs.
function ecs.session.openPreset(nameOrPath) end

---A preset or a saved session in a list.
---@class ecs.SessionEntry
---@field name string The file's name without .lua. For a preset, the name that ecs.session.openPreset takes.
---@field path string The file's path.
---@field appID string The tool's app id.
---@field appName string The tool's name.
---@field lastUsed? integer Presets only: when the tool's last session was written, in seconds since 1970. Missing if it has none.
---@field saved? integer Sessions only: when the file was written, in seconds since 1970.

---Lists the presets that a name finds: the user's presets, then the first-party presets. A name in both is listed once, from the user's presets.
---A file that cannot be read is reported and left out, and so is a preset with listed = false.
---@return ecs.SessionEntry[] presets Sorted by name.
function ecs.session.listPresets() end

---Lists the .lua files in the folder of saved sessions. A file that cannot be read is reported and left out.
---@return ecs.SessionEntry[] sessions Sorted by name.
function ecs.session.listSessions() end

-- Keys

---@class ecs.key
ecs.key = {}

---Gives a function a default key: for one of the plugin's panel types, or, with the scope "prefix", after the core prefix for one of the plugin's own functions.
---The core's keys after the prefix win over a plugin's. Presets and the user's settings can change it.
---@param scope string One of the plugin's panel types, by its full name, such as "canvas.view", or "prefix".
---@param key string A key combination, such as "Ctrl+E".
---@param functionName string A service function, such as "canvas.clear".
---A panel type, or a function after the prefix, that is not the plugin's, or a text that is not a key combination, is a misuse (ecs.check).
function ecs.key.bind(scope, key, functionName) end

---A key binding of the keys tables in effect, as ecs.key.list gives it.
---@class ecs.KeyBinding
---@field key string The key combination, as written.
---@field function string|false The function it runs, or false for a key that it removes from the layers below.
---@field scope string? nil for the whole tool, "prefix" for the keys after the core prefix, or a panel type.
---@field layer "default"|"preset"|"user"
---@field workspace boolean It comes from the current workspace's keys.
---@field active boolean It is the binding of its key that runs in its scope.

---A function that a key can run, as ecs.key.functions gives it.
---@class ecs.KeyFunction
---@field name string
---@field description string
---@field owner string Its plugin's name, or "ecs" for the core's.
---@field panel boolean It takes the focused panel.

---Lists every key binding of the keys tables in effect: the core's settings file, the plugins' keys for their panel types, the preset, the current
---workspace and the user's settings (DESIGN 7.8).
---@return ecs.KeyBinding[] bindings
function ecs.key.list() end

---Changes a key in the user's settings file and writes the file. The tool's own part of the file is changed if it binds the key in the scope already,
---and the part for every tool otherwise.
---@param scope string? nil for the whole tool, "prefix" for the keys after the core prefix, or a panel type, such as "canvas.view".
---@param key string A key combination, such as "Ctrl+E".
---@param functionName string|false|nil A function's name binds the key; false removes it from the layers below; nil takes the user's binding away.
---@return true|nil ok true if the key is set and written.
---@return string? message Why the key is not set.
---A text that is not a key combination, or a scope or function of another kind, is a misuse (ecs.check).
function ecs.key.set(scope, key, functionName) end

---Lists the functions that a key can run: those that take no arguments, or the focused panel.
---@return ecs.KeyFunction[] functions
function ecs.key.functions() end

-- Events

---@class ecs.Subscription
local Subscription = {}

---Ends the subscription. One that already ended does nothing.
function Subscription:unsubscribe() end

---@class ecs.event
ecs.event = {}

---Declares an event that the plugin emits.
---@param name string The local name, such as "changed"; the event is "canvas.changed".
---@param description string What the event says, for the user and for other plugins.
function ecs.event.declare(name, description) end

---Emits a declared event of the plugin.
---@param name string The full name, such as "canvas.changed".
---@param value? any The event's value, which each subscriber gets.
function ecs.event.emit(name, value) end

---Subscribes to an event, such as "ecs.panelOpened".
---@param name string The event's full name, such as "canvas.changed".
---@param callback fun(name: string, value: any) Gets the event's name and value, each time it is emitted.
---@return ecs.Subscription subscription The subscription, which ends it.
function ecs.event.subscribe(name, callback) end

-- Handles

---A value that stands for an object of a plugin, typed by name, such as "canvas.brush".
---@class ecs.Handle

---@class ecs.handle
ecs.handle = {}

---Registers a handle type of the plugin.
---@param name string The local name, such as "brush"; signatures name it "handle<canvas.brush>".
function ecs.handle.registerType(name) end

---Makes a function of the plugin a method of one of its handle types, so a handle's user calls it on the handle: `socket:send(data, true)`.
---@param type string The handle type's local name, such as "brush".
---@param method string The method's name, such as "paint".
---@param functionName string The local name of one of the plugin's functions, registered with ecs.service.register, whose first parameter is a handle of the type.
function ecs.handle.addMethod(type, method, functionName) end

---Makes a handle that stands for a Lua value. Lua only: in C, a handle is the object's pointer.
---@param name string A handle type of the plugin, by its local name, such as "brush".
---@param value any The Lua value that the handle stands for.
---@return ecs.Handle handle The new handle.
function ecs.handle.new(name, value) end

---Gives back the value of a handle of the plugin's type. Lua only: in C, a handle is the object's pointer.
---@param handle ecs.Handle A handle that ecs.handle.new made.
---@param name string The handle's type, by its local name.
---@return any value The Lua value the handle stands for.
function ecs.handle.value(handle, name) end

-- Structs

---Plain data with fixed fields, typed by name, such as "geometry.vec2": cdata of its type's own FFI type, laid out as C, with its type's methods and operators. A function also takes a table of its fields.
---@class ecs.Struct: { [string]: any }

---@class ecs.struct
ecs.struct = {}

---Registers a struct type of the plugin, before the functions whose signatures name it.
---@param name string The local name, such as "vec2"; signatures name it "struct<geometry.vec2>".
---@param fields string Its fields in order, as a signature writes parameters: "float x, float y". A field may be a list of a fixed length, "float m[16]", or another struct, "struct<geometry.vec2> position"; a struct of one list without a name, "float[16]", is that list. Fields are bool, int, int64, float, double, color or struct.
function ecs.struct.register(name, fields) end

---Makes a function of the plugin a method of one of its struct types, so Lua calls it on a struct: `a:length()`. A method named add, sub, mul, div, mod, pow, unm, concat, eq, lt, le, len or tostring is also Lua's operator: `a + b` calls add.
---@param type string The struct type's local name, such as "vec2".
---@param method string The method's name, such as "length".
---@param functionName string The local name of one of the plugin's functions, registered with ecs.service.register, whose first parameter is a struct of the type.
function ecs.struct.addMethod(type, method, functionName) end

---Makes a struct from a table of its fields, or a copy of a struct of its type. Other fields of the table are left out.
---@param name string The struct type, by its full name, such as "geometry.vec2".
---@param fields table|ecs.Struct A table with every field of the type, or a struct of the type.
---@return ecs.Struct struct The new struct.
function ecs.struct.new(name, fields) end

-- Services

---@class ecs.ServiceFunction
---@field sig string The signature, such as "int(string count, float scale)". Types: void, bool, int, int64, float, double, color, string, buffer, value, handle<name>, struct<name>, fn<signature>, out before a type, and keep before a callback that the function keeps after it returns.
---@field doc? string One line that says what the function does, which menus show. Lines after it say what its parameters and result are: "@param name text" for a parameter the signature names, and "@return text".
---@field fn function

---@class ecs.service
ecs.service = {}

---Registers functions of the plugin, by their local names: { clear = {...} } of the plugin canvas registers "canvas.clear".
---@param functions table<string, ecs.ServiceFunction> The functions, by their local names, each with its signature, description and Lua function.
---A registration that is not valid, such as a bad signature or a second one of a name, is a misuse (ecs.check).
function ecs.service.register(functions) end

---Gets a registered function.
---@param name string Such as "canvas.clear".
---@param signature? string The signature the caller expects.
---@return function? callback The function, to call as any Lua function.
---@return string? message Why the function is not available.
function ecs.service.get(name, signature) end

-- Plugin

---A plugin's own life, apart from its panels: the state it saves in sessions, and what it does before the program exits.
---A plugin is created when its code runs at load, and destroyed before the program exits, as a panel is created when it opens and destroyed when it closes.
---@class ecs.PluginDesc
---@field stateVersion? integer The version of the state it saves now; restoreState gets the version the state was saved with.
---@field saveState? fun(): any Gives the state to save in the session.
---@field restoreState? fun(saved: any, version: integer) Restores the state of a session, before its panels are created.
---@field destroy? fun() Runs before the program exits, while timers and events still work.

---@class ecs.plugin
---@field name string
---@field version string
---@field folder string The folder the plugin was loaded from, ending with a separator, such as for its images.
ecs.plugin = {}

---Registers the plugin's description, once. saveState and restoreState go together.
---@param desc ecs.PluginDesc The plugin's state version and functions.
function ecs.plugin.register(desc) end

-- Clipboard

---@class ecs.clipboard
ecs.clipboard = {}

---Puts text on the system's clipboard.
---@param text string The text, UTF-8.
---@return boolean ok true if the clipboard took the text.
function ecs.clipboard.setText(text) end

---Gives the text on the system's clipboard.
---@return string text The clipboard's text, or an empty text if it holds none.
function ecs.clipboard.getText() end

---Puts data of a type on the system's clipboard, such as a PNG image.
---@param mimeType string Such as "image/png".
---@param bytes string|string.buffer|ffi.cdata* The data.
---@return boolean ok true if the clipboard took the data.
function ecs.clipboard.setData(mimeType, bytes) end

---Gives the clipboard's data of a type.
---@param mimeType string Such as "image/png".
---@return string? bytes The data, or nil if the clipboard holds none of that type.
function ecs.clipboard.getData(mimeType) end

-- Dialogs

---@class ecs.DialogFilter
---@field name string Such as "Images".
---@field pattern string Such as "png;jpg".

---@class ecs.DialogDesc
---@field type? "openFile"|"saveFile"|"openFolder" "openFile" if missing.
---@field filters? ecs.DialogFilter[]
---@field location? string The folder or file the dialog starts at.
---@field many? boolean Lets the user choose more than one.

---@class ecs.dialog
ecs.dialog = {}

---Shows a file dialog. done gets the chosen paths, or nil if the user cancelled.
---@param desc ecs.DialogDesc The dialog's type, filters, start location and whether it takes many.
---@param done fun(files: string[]?) Gets the chosen paths, or nil if the user cancelled, on the main thread.
---@return true|nil ok true if the dialog is shown.
---@return string? message Why the dialog is not shown.
function ecs.dialog.show(desc, done) end

---Shows a message in a dialog that the core draws, and waits for a button; no plugin code runs while it waits.
---Return presses the first button unless the user chooses another, and Escape or closing the window presses the last.
---@param title string The dialog's title.
---@param message string The message.
---@param buttons? string[] { "OK" } if missing.
---@return integer? button The pressed button's position, from 1; nil if the dialog cannot be shown.
function ecs.dialog.showMessage(title, message, buttons) end

-- Files: the shapes of manifests, presets, sessions and settings files, for editors. Write ---@type ecs.Manifest, for example, above the file's return.

---A plugin's manifest.lua.
---@class ecs.Manifest
---@field name string The plugin's name, which everything it registers starts with.
---@field version string Its version, such as "1.2.0".
---@field api integer The plugin API version it was written for.
---@field description? string
---@field depends? table<string, string> The plugins it needs, each with the lowest version it accepts, such as { ui = "0.1" }.
---@field type "native"|"lua" Where its code is: "native" is its library in its folder, named as the platform names libraries, such as libcanvas.so; "lua" is init.lua in its folder.

---What a tool is called and how the desktop knows it.
---@class ecs.AppDesc
---@field id string Names the tool's last session and its .desktop file, such as "org.example.Paint".
---@field name string
---@field icon? string The icon's path, relative to the preset's folder.
---@field categories? string[] The categories of the tool's desktop entry, such as { "Graphics" }; OpenECS --install writes them.
---@field mimeTypes? string[] The file types that the preset's open function takes, such as { "image/png" }, for the desktop's "Open with".

---A panel in a preset or session.
---@class ecs.PanelDesc
---@field type string The panel type, such as "canvas.view".
---@field state? any The saved state it starts from.
---@field stateVersion? integer The version of the state.
---@field id? integer The panel's id, which sessions keep.

---A node of a layout tree: a split, with its children as list items, or a group, with its panels.
---@class ecs.LayoutNode
---@field [integer] ecs.LayoutNode A split's children, in order.
---@field split? "horizontal"|"vertical" Makes the node a split.
---@field size? number A fixed size in layout units, along the parent split.
---@field share? number A share of the parent split's remaining space.
---@field panels? ecs.PanelDesc[] Makes the node a group.
---@field shown? integer The group's shown panel, from 1.
---@field locked? boolean The user cannot move or close the group's panels.
---@field maximized? boolean The group fills its OS window.
---@field width? number A pop-out window's width, in layout units.
---@field height? number A pop-out window's height, in layout units.

---A keys table: a key combination runs the function it names, and false removes a key that a lower layer binds.
---prefix holds the keys after the core prefix, and a field named after a panel type the keys that work while such a panel has focus.
---@class ecs.Keys
---@field prefix? table<string, string|false>
---@field [string] string|false|table<string, string|false>

---A workspace of a preset or session.
---@class ecs.WorkspaceDesc
---@field name string
---@field windows ecs.LayoutNode[] One layout tree for each OS window: the main window first, then the pop-out windows.
---@field keys? ecs.Keys Keys for this workspace.
---@field focus? integer The id of the focused panel.

---A preset, which describes a tool.
---@class ecs.Preset
---@field format integer The version of the file format.
---@field name string
---@field version string
---@field app ecs.AppDesc
---@field depends table<string, string> The plugins the tool needs, each with the lowest version it accepts.
---@field pluginsDir? string A folder looked in for plugins first, relative to the preset.
---@field open? string The function that files from the command line go to; its signature is void(string).
---@field settings? table<string, any> Values of settings, over the plugins' defaults.
---@field keys? ecs.Keys Keys for the whole tool.
---@field listed? boolean false leaves the preset out of the list of presets.
---@field hint? string What to try first, such as keys to press; the title bar that OpenECS draws shows it after the tool's name.
---@field workspaces ecs.WorkspaceDesc[]

---A session, which OpenECS writes: a preset with the state it was saved in.
---@class ecs.Session: ecs.Preset
---@field currentWorkspace? integer
---@field pluginState? table<string, { state: any, stateVersion: integer }> Each plugin's own state.
---@field enabled? string[] Plugins the user enabled that the preset does not depend on.
---@field disabled? string[] Plugins the user disabled, with the plugins that depend on them.

---The user's settings file, or the core's settings file. A field whose name has a dot is a setting.
---@class ecs.SettingsFile
---@field [string] any
---@field keys? ecs.Keys Keys for every tool.
---@field plugins? string[] Plugins that every tool loads.
---@field tools? table<string, table<string, any>> Settings for one tool, by its app id.
---@field themes? table<string, table<string, any>> Values of settings for each theme, by its name, "dark" or "light", which add to the core's.

-- utf8, which plugins have from lua-compat-5.3 as Lua 5.3 has it, and LuaJIT lacks (DESIGN 9.6)

---@class utf8lib
---@field charpattern string The pattern of one UTF-8 character.
utf8 = {}

---Gives the UTF-8 text of code points.
---@param ... integer
---@return string
function utf8.char(...) end

---Gives an iterator over a text's characters: their positions and code points.
---@param text string
---@return fun(text: string, position: integer): integer, integer
---@return string
---@return integer
function utf8.codes(text) end

---Gives the code points of the characters that start between two positions.
---@param text string
---@param i? integer 1 if missing.
---@param j? integer i if missing.
---@return integer ...
function utf8.codepoint(text, i, j) end

---Gives the number of characters that start between two positions, or nil and the position of the first invalid byte.
---@param text string
---@param i? integer 1 if missing.
---@param j? integer -1 if missing.
---@return integer?
---@return integer? invalid
function utf8.len(text, i, j) end

---Gives the position where the n-th character, counted from i, starts.
---@param text string
---@param n integer
---@param i? integer 1 if n is 0 or more, #text + 1 otherwise.
---@return integer?
function utf8.offset(text, n, i) end

return ecs
