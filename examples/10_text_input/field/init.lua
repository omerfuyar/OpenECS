-- A one-line text field. Text comes in text events, which also carry what input methods compose, such as accented letters.
-- Keys that change the text, such as Backspace, come as keyDown events.

local ecs = require("ecs")

local fill = assert(ecs.service.get("ui.fill", "void(handle<ecs.surface>, float, float, float, float, int64)"))
local text = assert(ecs.service.get("ui.text", "float(handle<ecs.surface>, string, float, float, float, int64)"))
local measure = assert(ecs.service.get("ui.measure", "void(string, float, out float, out float)"))
local color = assert(ecs.service.get("ui.color", "int64(string)"))

local MARGIN, SIZE = 16, 20 -- in layout units

-- tells the core where the cursor is, in surface pixels, so an input method's window shows beside it
local function placeCursor(field)
  local width, height = measure(field.text, SIZE)
  field.panel:setTextInput(true, (MARGIN + width) * field.scale, MARGIN * field.scale, 2 * field.scale, height * field.scale)
end

ecs.panel.registerType({
  name = "field.panel",
  title = "Field",
  stateVersion = 1,

  create = function(panel, saved)
    local field = { panel = panel, text = saved and saved.text or "", scale = 1 }
    placeCursor(field) -- the panel gets text events only while it has the focus and accepts text
    return field
  end,

  draw = function(field, surface)
    field.scale = surface.scale
    fill(surface, 0, 0, surface.width / surface.scale, surface.height / surface.scale, color("background"))
    local width = text(surface, field.text, MARGIN, MARGIN, SIZE, color("text"))
    local _, height = measure(field.text, SIZE)
    fill(surface, MARGIN + width, MARGIN, 2, height, color("accent")) -- the cursor
  end,

  event = function(field, event)
    if event.type == "text" then
      field.text = field.text .. event.text
    elseif event.type == "keyDown" and event.key == "Backspace" and #field.text > 0 then
      field.text = field.text:sub(1, utf8.offset(field.text, -1) - 1) -- drops the last character, which may be several bytes
    else
      return
    end

    placeCursor(field)
    field.panel:redraw()
  end,

  saveState = function(field)
    return { text = field.text }
  end,
})
