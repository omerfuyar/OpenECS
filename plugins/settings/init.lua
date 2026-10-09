-- The settings window: lists every setting with its value, and changes it in the user's settings file (DESIGN 12.4).
-- It draws with the ui standard plugin.

local ecs = require("ecs")

local ui = require("ui")
local fill, text, measure, color = ui.fill, ui.text, ui.measure, ui.color
local button, check, field, scrollbar, thumb = ui.button, ui.check, ui.field, ui.scrollbar, ui.thumb

local function name(localName)
  return "settings." .. localName
end

-- sizes in layout units
local MARGIN = 24
local TITLE_SIZE = 22
local SECTION_SIZE = 13
local NAME_SIZE = 15
local NOTE_SIZE = 12
local ROW_PADDING = 8
local CONTROL_HEIGHT = 24
local CHECK_SIZE = 18
local VALUE_WIDTH = 260
local SCROLLBAR_WIDTH = 8
local WHEEL_STEP = 48

-- version of a window's saved state: the setting that was chosen
local STATE_VERSION = 1

-- the types whose values a step changes; every other type is typed
local STEPPED = { bool = true, choice = true, integer = true, number = true }

local windows = {} -- every open window's state, by its panel handle

-- the core's settings first, then each plugin's, each by name
local function readSettings()
  local names = ecs.settings.list() or {}
  local settings = {}

  for _, settingName in ipairs(names) do
    settings[#settings + 1] = ecs.settings.explain(settingName)
  end

  table.sort(settings, function(a, b)
    if (a.owner == "ecs") ~= (b.owner == "ecs") then
      return a.owner == "ecs"
    end

    return a.name < b.name
  end)

  return settings
end

-- a list or table as a Lua literal, which the user can type back
local function literal(value)
  if type(value) == "string" then
    return ("%q"):format(value)
  elseif type(value) ~= "table" then
    return tostring(value)
  end

  local parts = {}

  for _, item in ipairs(value) do
    parts[#parts + 1] = literal(item)
  end

  local keys = {}

  for key in pairs(value) do
    if math.type(key) ~= "integer" or key < 1 or key > #value then
      keys[#keys + 1] = key
    end
  end

  table.sort(keys, function(a, b)
    return tostring(a) < tostring(b)
  end)

  for _, key in ipairs(keys) do
    local shown = type(key) == "string" and key:match("^[%a_][%w_]*$") and key or ("[%s]"):format(literal(key))
    parts[#parts + 1] = shown .. " = " .. literal(value[key])
  end

  return #parts == 0 and "{}" or "{ " .. table.concat(parts, ", ") .. " }"
end

-- a value as the text of its control
local function show(value)
  if type(value) == "table" then
    return literal(value)
  elseif math.type(value) == "float" then
    return ("%g"):format(value)
  end

  return tostring(value)
end

-- reads typed text as a value of a setting's type
-- returns the value, or nil and why it is not one
local function read(setting, typed)
  if setting.type == "integer" then
    local value = math.tointeger(tonumber(typed))
    return value, value == nil and "a whole number" or nil
  elseif setting.type == "number" then
    local value = tonumber(typed)
    return value, value == nil and "a number" or nil
  elseif setting.type == "list" or setting.type == "table" then
    -- a Lua literal, read without access to anything
    local chunk = load("return " .. typed, "=" .. setting.name, "t", {})
    local ok, value = false, nil

    if chunk then
      ok, value = pcall(chunk)
    end

    if ok and type(value) == "table" then
      return value
    end

    return nil, "a Lua table, such as { 1, 2 }"
  end

  return typed
end

-- what the second line of a setting says
local function note(setting)
  return setting.description
end

local function lineHeight(size)
  local _, line = measure("Ag", size)
  return line
end

local function headerHeight()
  return MARGIN + lineHeight(TITLE_SIZE) + MARGIN / 2
end

local function rowHeight()
  return ROW_PADDING + CONTROL_HEIGHT + lineHeight(NOTE_SIZE) + ROW_PADDING
end

local function sectionHeight()
  return MARGIN / 2 + lineHeight(SECTION_SIZE) + 4
end

-- places every section title and row below the header: window.rows holds their tops and the setting they show
local function place(window)
  local rows, y, owner = {}, 0, nil

  for i, setting in ipairs(window.settings) do
    if setting.owner ~= owner then
      owner = setting.owner
      rows[#rows + 1] = { top = y, section = owner == "ecs" and "Core" or owner }
      y = y + sectionHeight()
    end

    rows[#rows + 1] = { top = y, index = i }
    y = y + rowHeight()
  end

  window.rows = rows
  window.total = y + MARGIN
end

local function refresh(window)
  local chosen = window.settings[window.selected]
  window.settings = readSettings()
  window.selected = math.min(math.max(window.selected, 1), math.max(#window.settings, 1))

  for i, setting in ipairs(window.settings) do
    if chosen and setting.name == chosen.name then
      window.selected = i
    end
  end

  place(window)
  window.panel:redraw()
end

-- the scroll offset within its bounds
local function clamp(window)
  window.offset = math.max(0, math.min(window.offset, window.total - window.shown))
end

-- scrolls so the chosen setting is in view
local function reveal(window)
  for _, row in ipairs(window.rows) do
    if row.index == window.selected then
      window.offset = math.min(math.max(window.offset, row.top + rowHeight() - window.shown), row.top)
    end
  end

  clamp(window)
end

-- the next value of a setting one step forward or back, or nil if the window does not step its type
local function step(setting, direction)
  local value = setting.value

  if setting.type == "bool" then
    return not value
  elseif setting.type == "choice" then
    local choices = setting.choices or {}

    for i, choice in ipairs(choices) do
      if choice == value then
        return choices[(i - 1 + direction) % #choices + 1]
      end
    end

    return choices[1]
  elseif setting.type == "integer" then
    return math.tointeger(value + direction)
  elseif setting.type == "number" then
    return value + direction
  end
end

local function findWindow(panel)
  local window = windows[panel]

  if not window then
    ecs.log.warn("That panel is not a settings window.")
  end

  return window
end

-- ends typing a value without saving it
local function cancel(window)
  if window.editing then
    window.editing = nil
    window.panel:setTextInput(false)
    window.panel:redraw()
  end
end

local function choose(window, index)
  cancel(window)
  window.selected = math.min(math.max(index, 1), #window.settings)
  reveal(window)
  window.panel:redraw()
end

local function set(window, setting, value)
  local ok, message = ecs.settings.set(setting.name, value)

  if not ok then
    ecs.log.warn(message or ("'%s' is not changed."):format(setting.name))
  end

  refresh(window)
end

-- changes the chosen setting one step, in the user's settings file
local function change(window, direction)
  cancel(window)
  local setting = window.settings[window.selected]
  local value = setting and step(setting, direction)

  if value ~= nil then
    set(window, setting, value)
  end
end

-- toggles or cycles the chosen setting, or starts typing its value, or saves the value typed
local function edit(window)
  local setting = window.settings[window.selected]

  if not setting then
    return
  elseif setting.type == "bool" or setting.type == "choice" then
    change(window, 1)
    return
  elseif not window.editing then
    window.editing = { text = show(setting.value) }
    window.panel:setTextInput(true)
    window.panel:redraw()
    return
  end

  local value, expected = read(setting, window.editing.text)

  if value == nil then
    ecs.log.warn(("'%s' needs %s."):format(setting.name, expected))
    return
  end

  cancel(window)
  set(window, setting, value)
end

-- the rectangle of a row's control, in the panel
local function controlRect(window, row, setting)
  local x = window.width - MARGIN - SCROLLBAR_WIDTH - window.valueWidth
  local y = window.top + row.top - window.offset + ROW_PADDING

  if setting.type == "bool" then
    return x, y + (CONTROL_HEIGHT - CHECK_SIZE) / 2, CHECK_SIZE, CHECK_SIZE
  end

  return x, y, window.valueWidth, CONTROL_HEIGHT
end

-- the row under a point in the panel, if any
local function rowAt(window, y)
  local inside = y - window.top + window.offset

  for _, row in ipairs(window.rows) do
    if row.index and inside >= row.top and inside < row.top + rowHeight() then
      return row
    end
  end
end

local function drawRow(window, surface, row)
  local setting = window.settings[row.index]
  local y = window.top + row.top - window.offset
  local chosen = row.index == window.selected

  if chosen then
    fill(surface, MARGIN / 2, y, window.width - MARGIN - SCROLLBAR_WIDTH, rowHeight(), color("selected"))
  end

  local nameY = y + ROW_PADDING + (CONTROL_HEIGHT - lineHeight(NAME_SIZE)) / 2
  text(surface, setting.name, MARGIN, nameY, NAME_SIZE, color("text"))
  text(surface, note(setting), MARGIN, y + ROW_PADDING + CONTROL_HEIGHT, NOTE_SIZE, color("textDim"))

  local x, top, width, height = controlRect(window, row, setting)

  if setting.type == "bool" then
    check(surface, x, top, width, setting.value == true)
  elseif setting.type == "choice" then
    button(surface, show(setting.value), x, top, width, height, chosen and 1 or 0)
  elseif chosen and window.editing then
    -- the cursor is where the input method shows its window
    local cursor = field(surface, window.editing.text, x, top, width, height, true)
    local scale = surface.scale
    window.panel:setTextInput(true, cursor * scale, top * scale, 2 * scale, height * scale)
  else
    field(surface, show(setting.value), x, top, width, height, false)
  end
end

local function draw(window, surface)
  window.width, window.height = surface.width / surface.scale, surface.height / surface.scale
  window.top = headerHeight()
  window.shown = window.height - window.top
  window.valueWidth = math.min(VALUE_WIDTH, window.width * 0.4)
  clamp(window)

  fill(surface, 0, 0, window.width, window.height, color("background"))

  for _, row in ipairs(window.rows) do
    local y = window.top + row.top - window.offset

    if y + rowHeight() > window.top and y < window.height then
      if row.section then
        text(surface, row.section, MARGIN, y + MARGIN / 2, SECTION_SIZE, color("accent"))
      else
        drawRow(window, surface, row)
      end
    end
  end

  -- the header covers the rows scrolled under it
  fill(surface, 0, 0, window.width, window.top, color("background"))
  text(surface, "Settings", MARGIN, MARGIN, TITLE_SIZE, color("text"))
  scrollbar(surface, window.width - SCROLLBAR_WIDTH - 4, window.top, SCROLLBAR_WIDTH, window.shown, window.total, window.shown, window.offset)
end

-- a press: on the scrollbar, it grabs the thumb or jumps there; on a row, it chooses the setting and uses its control
local function press(window, x, y)
  if y < window.top then
    return
  end

  if x >= window.width - SCROLLBAR_WIDTH - 8 and window.total > window.shown then
    local start, length = thumb(window.shown, window.total, window.shown, window.offset)
    local along = y - window.top

    -- a press beside the thumb centres the thumb there
    if along < start or along > start + length then
      local travel = window.shown - length
      window.offset = travel > 0 and (along - length / 2) / travel * (window.total - window.shown) or 0
      clamp(window)
      start = thumb(window.shown, window.total, window.shown, window.offset)
    end

    window.dragging = along - start
    window.panel:redraw()
    return
  end

  local row = rowAt(window, y)

  if not row then
    return
  end

  local left, top, width, height = controlRect(window, row, window.settings[row.index])
  local onControl = x >= left and x < left + width and y >= top and y < top + height

  -- a press on the field being typed in keeps typing
  if onControl and window.editing and row.index == window.selected then
    return
  end

  choose(window, row.index)

  if onControl then
    edit(window)
  end
end

-- a pointer move while the thumb is held scrolls the content with it
local function drag(window, y)
  local _, length = thumb(window.shown, window.total, window.shown, window.offset)
  local travel = window.shown - length

  if travel > 0 then
    window.offset = (y - window.top - window.dragging) / travel * (window.total - window.shown)
    clamp(window)
    window.panel:redraw()
  end
end

ecs.panel.registerType({
  name = name("window"),
  title = "Settings",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local window = { panel = panel, settings = {}, rows = {}, selected = 1, offset = 0, total = 0, shown = 0, top = 0, width = 0, height = 0, valueWidth = 0 }
    windows[panel] = window
    refresh(window)

    if version == STATE_VERSION and saved and saved.selected then
      for i, setting in ipairs(window.settings) do
        if setting.name == saved.selected then
          window.selected = i
        end
      end
    end

    return window
  end,
  destroy = function(window)
    windows[window.panel] = nil
  end,
  saveState = function(window)
    local chosen = window.settings[window.selected]
    return { selected = chosen and chosen.name or nil }
  end,
  draw = draw,
  event = function(window, event)
    if event.type == "text" and window.editing then
      window.editing.text = window.editing.text .. event.text
      window.panel:redraw()
    elseif event.type == "keyDown" and window.editing and event.key == "Backspace" then
      -- the last character, which may take several bytes
      local last = utf8.offset(window.editing.text, -1)
      window.editing.text = last and window.editing.text:sub(1, last - 1) or ""
      window.panel:redraw()
    elseif event.type == "keyDown" and window.editing and event.key == "Escape" then
      cancel(window)
    elseif event.type == "shown" then
      refresh(window)
    elseif event.type == "pointerDown" and event.button == 1 then
      press(window, event.x, event.y)
    elseif event.type == "pointerMove" and window.dragging then
      drag(window, event.y)
    elseif event.type == "pointerUp" then
      window.dragging = nil
    elseif event.type == "wheel" then
      window.offset = window.offset - event.wheelY * WHEEL_STEP
      clamp(window)
      window.panel:redraw()
    end
  end,
})

local services = {}

-- opens the settings window, or shows the one that is open
function services.open()
  local open = next(windows)

  if open then
    ecs.layout.focus(open)
    return
  end

  local panel, message = ecs.layout.open(name("window"))

  if not panel then
    ecs.log.warn(message or "Cannot open the settings window.")
  end
end

-- each function acts on the settings window it gets
local function forWindow(act)
  return function(panel)
    local window = findWindow(panel)

    if window and #window.settings > 0 then
      act(window)
    end
  end
end

services.up = forWindow(function(window)
  choose(window, window.selected - 1)
end)

services.down = forWindow(function(window)
  choose(window, window.selected + 1)
end)

services.previous = forWindow(function(window)
  change(window, -1)
end)

services.next = forWindow(function(window)
  change(window, 1)
end)

services.edit = forWindow(edit)

assert(ecs.service.register("settings", {
  open = { sig = "void()", doc = "Open the settings window", fn = services.open },
  up = { sig = "void(handle<ecs.panel>)", doc = "Choose the setting above", fn = services.up },
  down = { sig = "void(handle<ecs.panel>)", doc = "Choose the setting below", fn = services.down },
  previous = { sig = "void(handle<ecs.panel>)", doc = "Change the chosen setting one step back", fn = services.previous },
  next = { sig = "void(handle<ecs.panel>)", doc = "Change the chosen setting one step forward", fn = services.next },
  edit = { sig = "void(handle<ecs.panel>)", doc = "Toggle or cycle the chosen setting, or type its value, or save the value typed", fn = services.edit },
}))

-- default keys for the window; presets and the user's settings can change them
for service, key in pairs({ up = "Up", down = "Down", previous = "Left", next = "Right", edit = "Return" }) do
  assert(ecs.input.bind(name("window"), key, name(service)))
end
