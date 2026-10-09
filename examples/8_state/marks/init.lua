-- Panels that keep the dots clicked on them. Sessions save each panel's dots, and the plugin's count of every dot placed.
-- A session is a tool as it was left: OpenECS saves it when the tool quits and builds the tool from it next time.

local ecs = require("ecs")

local fill = assert(ecs.service.get("ui.fill", "void(handle<ecs.surface>, float, float, float, float, int64)"))
local color = assert(ecs.service.get("ui.color", "int64(string)"))

-- raise it when the saved state changes shape; create then gets the old version, and can read or drop the old state
local STATE_VERSION = 1
local DOT = 8 -- in layout units

local placed = 0 -- every dot placed, in every session; the plugin's own state

ecs.panel.registerType({
  name = "marks.panel",
  title = "Marks",
  stateVersion = STATE_VERSION,

  -- saved is the state saveState gave last time, or nil for a new panel
  create = function(panel, saved, version)
    local marks = { panel = panel, dots = {}, scale = 1 }

    if version == STATE_VERSION and saved then
      marks.dots = saved.dots
    end

    return marks
  end,

  draw = function(marks, surface)
    marks.scale = surface.scale
    fill(surface, 0, 0, surface.width / surface.scale, surface.height / surface.scale, color("background"))

    for i = 1, #marks.dots - 1, 2 do
      fill(surface, marks.dots[i] - DOT / 2, marks.dots[i + 1] - DOT / 2, DOT, DOT, color("accent"))
    end
  end,

  event = function(marks, event)
    if event.type == "pointerDown" and event.button == 1 then
      table.insert(marks.dots, event.x / marks.scale) -- dots are kept in layout units, so they stay put at any scale
      table.insert(marks.dots, event.y / marks.scale)
      placed = placed + 1
      marks.panel:setUnsaved(true) -- marks the tab; closing the panel then asks to save
      marks.panel:redraw()
    end
  end,

  -- gives what the session keeps: plain values only, such as numbers, strings and tables of them
  saveState = function(marks)
    return { dots = marks.dots }
  end,

  -- saves the panel's unsaved work, when the user asks or closes it; the dots are already in the session, so it only clears the mark
  save = function(marks)
    marks.panel:setUnsaved(false)
    return true
  end,
})

-- state that belongs to no panel; restore runs before the session's panels are made
ecs.plugin.registerState({
  version = 1,
  save = function()
    return { placed = placed }
  end,
  restore = function(saved)
    placed = saved and saved.placed or 0
  end,
})
