-- The settings window: lists every setting with its value and the layer it comes from, and changes the values it can (DESIGN 12.4).
-- It draws with the ui standard plugin.

local ecs = require("ecs")

local fill = assert(ecs.service.get("ui.fill", "void(handle<ecs.surface>, float, float, float, float, int64)"))
local text = assert(ecs.service.get("ui.text", "float(handle<ecs.surface>, string, float, float, float, int64)"))
local measure = assert(ecs.service.get("ui.measure", "void(string, float, out float, out float)"))
local color = assert(ecs.service.get("ui.color", "int64(string)"))

local function name(localName)
  return "settings." .. localName
end

-- sizes in layout units
local MARGIN = 24
local TITLE_SIZE = 22
local NAME_SIZE = 15
local NOTE_SIZE = 12
local ROW_PADDING = 6

-- version of a window's saved state: the setting that was chosen
local STATE_VERSION = 1

-- the types whose values the window changes: a step changes them
local EDITABLE = { bool = true, choice = true, integer = true, number = true }

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

-- a value as one line of text
local function show(value)
  if type(value) == "boolean" then
    return value and "on" or "off"
  elseif type(value) == "table" then
    return ("%d entries"):format(#value)
  elseif math.type(value) == "float" then
    return ("%g"):format(value)
  end

  return tostring(value)
end

-- what the second line of a setting says: where its value comes from, and whether the window can change it
local function note(setting)
  if setting.layer == "user" then
    return "set in " .. (setting.file or "the user's settings") .. ", which wins over this window"
  elseif not EDITABLE[setting.type] then
    return setting.description .. "; change it in your settings file"
  end

  return setting.description
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

  window.panel:redraw()
end

local function rowHeight()
  local _, nameLine = measure("Ag", NAME_SIZE)
  local _, noteLine = measure("Ag", NOTE_SIZE)
  return nameLine + noteLine + 2 * ROW_PADDING
end

local function rowsTop()
  local _, title = measure("Ag", TITLE_SIZE)
  return MARGIN + title + MARGIN
end

-- the next value of a setting one step forward or back, or nil if the window does not change its type
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

local function move(panel, rows)
  local window = findWindow(panel)

  if window and #window.settings > 0 then
    window.selected = math.min(math.max(window.selected + rows, 1), #window.settings)
    panel:redraw()
  end
end

-- changes the chosen setting one step, in the settings window's layer
local function change(panel, direction)
  local window = findWindow(panel)
  local setting = window and window.settings[window.selected]
  local value = setting and step(setting, direction)

  if value == nil then
    return
  end

  local ok, message = ecs.settings.set(setting.name, value)

  if not ok then
    ecs.log.warn(message or ("'%s' is not changed."):format(setting.name))
  end

  refresh(window)
end

ecs.panel.registerType({
  name = name("window"),
  title = "Settings",
  stateVersion = STATE_VERSION,
  create = function(panel, saved, version)
    local window = { panel = panel, settings = {}, selected = 1, scroll = 0 }
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
  draw = function(window, surface)
    local width, height = surface.width / surface.scale, surface.height / surface.scale
    local textColor, dimColor = color("text"), color("textDim")

    fill(surface, 0, 0, width, height, color("background"))
    text(surface, "Settings", MARGIN, MARGIN, TITLE_SIZE, textColor)

    local row = rowHeight()
    local top = rowsTop()
    local _, nameLine = measure("Ag", NAME_SIZE)

    -- the chosen setting stays in view
    local visible = math.max(1, math.floor((height - top) / row))
    window.scroll = math.min(math.max(window.scroll, window.selected - visible), window.selected - 1)

    local valueX = math.max(MARGIN + 220, width * 0.5)
    local y = top

    for i = window.scroll + 1, math.min(#window.settings, window.scroll + visible) do
      local setting = window.settings[i]

      -- a value that the user's own file sets wins over this window, so it is faded
      local valueColor = setting.layer == "user" and dimColor or textColor

      if i == window.selected then
        fill(surface, MARGIN / 2, y, width - MARGIN, row, color("selected"))
      end

      text(surface, setting.name, MARGIN, y + ROW_PADDING, NAME_SIZE, textColor)
      text(surface, show(setting.value), valueX, y + ROW_PADDING, NAME_SIZE, valueColor)
      text(surface, note(setting), MARGIN, y + ROW_PADDING + nameLine, NOTE_SIZE, dimColor)
      y = y + row
    end
  end,
  event = function(window, event)
    if event.type == "shown" then
      refresh(window)
    elseif event.type == "pointerDown" and event.button == 1 and event.y >= rowsTop() then
      local index = window.scroll + math.floor((event.y - rowsTop()) / rowHeight()) + 1

      if window.settings[index] then
        window.selected = index
        window.panel:redraw()
      end
    elseif event.type == "wheel" then
      move(window.panel, event.wheelY > 0 and -1 or 1)
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

function services.up(panel)
  move(panel, -1)
end

function services.down(panel)
  move(panel, 1)
end

function services.previous(panel)
  change(panel, -1)
end

function services.next(panel)
  change(panel, 1)
end

assert(ecs.service.register("settings", {
  open = { sig = "void()", doc = "Open the settings window", fn = services.open },
  up = { sig = "void(handle<ecs.panel>)", doc = "Choose the setting above", fn = services.up },
  down = { sig = "void(handle<ecs.panel>)", doc = "Choose the setting below", fn = services.down },
  previous = { sig = "void(handle<ecs.panel>)", doc = "Change the chosen setting one step back", fn = services.previous },
  next = { sig = "void(handle<ecs.panel>)", doc = "Change the chosen setting one step forward", fn = services.next },
}))

-- keys are settings, so the user can change them
for setting, key in pairs({ upKey = "Up", downKey = "Down", previousKey = "Left", nextKey = "Right" }) do
  local service = setting:sub(1, -4)
  assert(ecs.settings.declare({ name = name(setting), type = "key", description = "Key that runs " .. name(service), default = key }))
  assert(ecs.input.bind(name("window"), name(setting), name(service)))
end
