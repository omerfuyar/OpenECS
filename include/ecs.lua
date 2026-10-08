---@meta ecs

-- The ecs module of OpenECS for Lua plugins: local ecs = require("ecs").
-- This file only describes the module for editors; OpenECS gives each plugin its own ecs table.

-- A function that keys can run says so. Keys call it with no arguments, which acts on the focused panel as the user does: locks stop it.

---@class ecs
---@field workspace1 fun() Switches to workspace 1. Keys can run it.
---@field workspace2 fun() Switches to workspace 2. Keys can run it.
---@field workspace3 fun() Switches to workspace 3. Keys can run it.
---@field workspace4 fun() Switches to workspace 4. Keys can run it.
---@field workspace5 fun() Switches to workspace 5. Keys can run it.
---@field workspace6 fun() Switches to workspace 6. Keys can run it.
---@field workspace7 fun() Switches to workspace 7. Keys can run it.
---@field workspace8 fun() Switches to workspace 8. Keys can run it.
---@field workspace9 fun() Switches to workspace 9. Keys can run it.
---@field workspace10 fun() Switches to workspace 10. Keys can run it.
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
local ecs = {}

-- Log

---@class ecs.log
ecs.log = {}

---Writes a debug message in the plugin's name.
---@param text string
function ecs.log.debug(text) end

---Writes an information message in the plugin's name.
---@param text string
function ecs.log.info(text) end

---Writes a warning in the plugin's name.
---@param text string
function ecs.log.warn(text) end

---Writes an error in the plugin's name.
---@param text string
function ecs.log.error(text) end

-- Settings

---@alias ecs.SettingType "bool"|"integer"|"number"|"string"|"choice"|"key"|"list"|"table"

---@class ecs.SettingDesc
---@field name string The plugin's name, a dot and the setting's name, such as "canvas.grid".
---@field type ecs.SettingType
---@field description? string
---@field choices? string[] The texts a "choice" setting can have.
---@field default? any
---@field changed? fun(value: any) Called with the new value when the setting changes.

---@class ecs.settings
ecs.settings = {}

---Declares a setting of the plugin.
---@param desc ecs.SettingDesc
---@return true|nil ok
---@return string? message Why the setting is not declared.
function ecs.settings.declare(desc) end

---Gets a setting's value from the highest layer that sets it.
---@param name string
---@return any value
function ecs.settings.get(name) end

---Sets a setting in the settings window's layer, and writes that layer's file. A higher layer may still override it.
---@param name string
---@param value any
---@return true|nil ok
---@return string? message Why the setting is not set.
function ecs.settings.set(name, value) end

---Lists the names of every declared setting, in the order they were declared.
---@return string[]? names
---@return string? message
function ecs.settings.list() end

---@class ecs.SettingExplanation
---@field name string
---@field type ecs.SettingType
---@field description string
---@field owner string The plugin that declared the setting.
---@field value any The value in effect.
---@field layer string The layer the value comes from.
---@field file? string The file of that layer; missing for defaults.
---@field choices? string[]
---@field layers { default: any, preset: any, window: any, user: any } The value of each layer that sets it.

---Explains a setting: its value in effect, the layer it comes from, and what each layer says.
---@param name string
---@return ecs.SettingExplanation? explanation
---@return string? message
function ecs.settings.explain(name) end

-- Timers

---@class ecs.Timer
local Timer = {}

---Stops the timer. A timer that already ended does nothing.
function Timer:stop() end

---@class ecs.timer
ecs.timer = {}

---Starts a timer of the plugin.
---@param seconds number More than 0, or 0 for a one-shot timer.
---@param repeat_ boolean
---@param fn fun()
---@return ecs.Timer
function ecs.timer.start(seconds, repeat_, fn) end

-- Panels

---@class ecs.Panel
local Panel = {}

---Asks for the panel to be drawn again.
function Panel:redraw() end

---@return string title
function Panel:getTitle() end

---@param text string
function Panel:setTitle(text) end

---@return integer id The panel's id, which stays the same across sessions.
function Panel:getId() end

---@return string type The name of the panel's type.
function Panel:getType() end

---Marks the panel as having unsaved work, which shows a mark on its tab.
---@param unsaved boolean
function Panel:setUnsaved(unsaved) end

---Starts a timer that stops when the panel is closed.
---@param seconds number
---@param repeat_ boolean
---@param fn fun()
---@return ecs.Timer
function Panel:startTimer(seconds, repeat_, fn) end

---The pixels a panel draws into. It is valid only while draw runs.
---@class ecs.Surface
---@field width integer
---@field height integer
---@field scale number Pixels per layout unit.
local Surface = {}

---Sets a pixel. Pixels outside the surface are clipped. Lua only: C writes the surface's pixels directly.
---@param x integer
---@param y integer
---@param color integer ARGB, such as 0xFFFF0000.
function Surface:setPixel(x, y, color) end

---Lua only: C reads the surface's pixels directly.
---@param x integer
---@param y integer
---@return integer color ARGB.
function Surface:getPixel(x, y) end

---Copies a row of pixels, clipped to the surface. Lua only: C writes the surface's pixels directly.
---@param y integer
---@param bytes string ARGB pixels, 4 bytes each in native byte order.
---@param x? integer The first pixel's column; 0 if missing.
function Surface:setRow(y, bytes, x) end

---@alias ecs.PanelEventType "pointerDown"|"pointerUp"|"pointerMove"|"wheel"|"keyDown"|"keyUp"|"focused"|"unfocused"|"shown"|"hidden"|"resized"

---@class ecs.PanelEvent
---@field type ecs.PanelEventType
---@field shift boolean
---@field ctrl boolean
---@field alt boolean
---@field super boolean
---@field x? number For pointer and wheel events.
---@field y? number For pointer and wheel events.
---@field button? integer For pointerDown and pointerUp.
---@field wheelX? number For wheel events.
---@field wheelY? number For wheel events.
---@field key? string SDL's key name, for keyDown and keyUp.
---@field width? number For shown and resized.
---@field height? number For shown and resized.

---@class ecs.PanelTypeDesc
---@field name string The plugin's name, a dot and the type's name, such as "canvas.view".
---@field title? string The default title of tabs and menus.
---@field surface? "pixels"|"gpu" "pixels" if missing.
---@field stateVersion? integer The version of the saved state.
---@field continuous? boolean Draw every frame while the panel is shown.
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
---@param desc ecs.PanelTypeDesc
---@return true|nil ok
---@return string? message Why the type is not registered.
function ecs.panel.registerType(desc) end

---Adds a service function to the panel menu of a type. Its signature is "void(handle<ecs.panel>)" or "void()".
---@param type string
---@param functionName string
---@return true|nil ok
---@return string? message
function ecs.panel.addMenuEntry(type, functionName) end

---@param panel ecs.Panel
function ecs.panel.redraw(panel) end

---@param panel ecs.Panel
---@return string title
function ecs.panel.getTitle(panel) end

---@param panel ecs.Panel
---@param text string
function ecs.panel.setTitle(panel, text) end

---@param panel ecs.Panel
---@return integer id The panel's id, which stays the same across sessions.
function ecs.panel.getId(panel) end

---@param panel ecs.Panel
---@return string type The name of the panel's type.
function ecs.panel.getType(panel) end

---@param panel ecs.Panel
---@param unsaved boolean
function ecs.panel.setUnsaved(panel, unsaved) end

---@param panel ecs.Panel
---@param seconds number
---@param repeat_ boolean
---@param fn fun()
---@return ecs.Timer
function ecs.panel.startTimer(panel, seconds, repeat_, fn) end

---Restarts the focused panel from its last saved state, if it failed. Keys can run it.
function ecs.panel.restart() end

-- Layout and workspaces

---@alias ecs.Zone "default"|"center"|"left"|"right"|"top"|"bottom"

---@class ecs.layout
ecs.layout = {}

---Finds a panel by its id.
---@param id integer
---@return ecs.Panel? panel
function ecs.layout.find(id) end

---Opens a panel.
---@param type string The panel type, such as "canvas.view".
---@param saved? any The saved state the panel starts from.
---@param target? ecs.Panel The panel to open it next to.
---@param zone? ecs.Zone Where, next to the target; "default" if missing.
---@return ecs.Panel? panel
---@return string? message Why the panel is not opened.
function ecs.layout.open(type, saved, target, zone) end

---Moves a panel next to another one in the same workspace.
---@param panel ecs.Panel
---@param target ecs.Panel
---@param zone? ecs.Zone "center" if missing.
---@return true|nil ok
---@return string? message
function ecs.layout.move(panel, target, zone) end

---Closes a panel. If it has unsaved work, the user is asked first and may cancel.
---Without a panel, it is the user's close: the focused panel, unless its group is locked. Keys can run it.
---@param panel? ecs.Panel
---@return boolean closed
function ecs.layout.close(panel) end

---@param panel ecs.Panel
function ecs.layout.focus(panel) end

---@return ecs.Panel? panel The focused panel.
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

---Closes the panels of the focused panel's group, unless it is locked. Keys can run it.
function ecs.layout.closeGroup() end

---Locks the focused panel's group, or unlocks it. Keys can run it.
function ecs.layout.lock() end

---Opens the last closed panel again. Keys can run it.
function ecs.layout.reopen() end

---Opens another panel of the focused panel's type on its right. Keys can run it.
function ecs.layout.splitRight() end

---Opens another panel of the focused panel's type below it. Keys can run it.
function ecs.layout.splitDown() end

---@class ecs.workspace
ecs.workspace = {}

---@return integer count
function ecs.workspace.count() end

---@return integer number The shown workspace, from 1.
function ecs.workspace.getCurrent() end

---@param number integer From 1.
---@return string? name
function ecs.workspace.getName(number) end

---@param number integer From 1.
function ecs.workspace.switch(number) end

-- Session

---@class ecs.session
ecs.session = {}

---Writes the session to a file: the plugins' state, the workspaces and the panels with their saved state. Quitting still saves the tool's last session.
---Without a path, it asks for the file with a save dialog. Keys can run it.
---@param path? string Missing folders are created.
---@return true|nil ok
---@return string? message Why the session is not saved.
function ecs.session.save(path) end

---Opens a session in place of the current one: it asks about unsaved work, and once the current pass of the main loop ends, OpenECS saves the tool's last session, stops and starts again from the session.
---Without a path, it asks for the file with an open dialog. Keys can run it.
---@param path? string
---@return true|nil ok
---@return string? message Why the session is not opened: the file is not a session, the user keeps the unsaved work, or a test runs.
function ecs.session.open(path) end

-- Input

---@class ecs.input
ecs.input = {}

---Binds a key setting of the plugin to a function, for the panels of a type.
---@param panelType string
---@param settingName string A setting of type "key".
---@param functionName string A service function, such as "canvas.clear".
---@return true|nil ok
---@return string? message
function ecs.input.bind(panelType, settingName, functionName) end

-- Events

---@class ecs.Subscription
local Subscription = {}

---Stops the subscription. One that already ended does nothing.
function Subscription:cancel() end

---@class ecs.event
ecs.event = {}

---Declares an event that the plugin emits.
---@param name string The plugin's name, a dot and the event's name.
---@param description? string
function ecs.event.declare(name, description) end

---Emits a declared event of the plugin.
---@param name string
---@param value? any
function ecs.event.emit(name, value) end

---Subscribes to an event, such as "ecs.panelOpened".
---@param name string
---@param fn fun(name: string, value: any)
---@return ecs.Subscription
function ecs.event.subscribe(name, fn) end

-- Handles

---A value that stands for an object of a plugin, typed by name, such as "canvas.brush".
---@class ecs.Handle

---@class ecs.handle
ecs.handle = {}

---Registers a handle type of the plugin.
---@param name string
function ecs.handle.registerType(name) end

---Makes a handle that stands for a Lua value. Lua only: in C, a handle is the object's pointer.
---@param name string A handle type of the plugin.
---@param value any
---@return ecs.Handle
function ecs.handle.new(name, value) end

---Gives back the value of a handle of the plugin's type. Lua only: in C, a handle is the object's pointer.
---@param handle ecs.Handle
---@param name string
---@return any value
function ecs.handle.value(handle, name) end

-- Services

---@class ecs.ServiceFunction
---@field sig string The signature, such as "int(string, float)".
---@field doc? string A one-line description.
---@field fn function

---@class ecs.service
ecs.service = {}

---Registers functions named "<prefix>.<local name>".
---@param prefix string Starts with the plugin's name.
---@param functions table<string, ecs.ServiceFunction>
---@return true|nil ok
---@return string? message Which function is not registered, and why.
function ecs.service.register(prefix, functions) end

---Gets a registered function.
---@param name string Such as "canvas.clear".
---@param signature? string The signature the caller expects.
---@return function? fn
---@return string? message Why the function is not available.
function ecs.service.get(name, signature) end

-- Plugin

---@class ecs.PluginStateDesc
---@field version? integer
---@field save fun(): any Gives the state to save in the session.
---@field restore fun(saved: any, version: integer)

---@class ecs.plugin
---@field name string
---@field version string
ecs.plugin = {}

---Saves state of the plugin in the session, apart from its panels'.
---@param desc ecs.PluginStateDesc
function ecs.plugin.registerState(desc) end

---Sets the function that runs when the plugin shuts down.
---@param fn fun()
function ecs.plugin.onShutdown(fn) end

-- Clipboard

---@class ecs.clipboard
ecs.clipboard = {}

---@param text string
---@return boolean ok
function ecs.clipboard.setText(text) end

---@return string text
function ecs.clipboard.getText() end

---@param mimeType string Such as "image/png".
---@param bytes string
---@return boolean ok
function ecs.clipboard.setData(mimeType, bytes) end

---@param mimeType string
---@return string? bytes
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
---@param desc ecs.DialogDesc
---@param done fun(files: string[]?)
---@return true|nil ok
---@return string? message Why the dialog is not shown.
function ecs.dialog.show(desc, done) end

---Shows a message and waits for a button.
---@param title string
---@param text string
---@param buttons? string[] { "OK" } if missing.
---@return integer? button The pressed button's position, from 1.
function ecs.dialog.message(title, text, buttons) end

return ecs
