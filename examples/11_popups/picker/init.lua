-- A panel whose colour a right-click menu chooses. The menu is a popup: it draws itself and gets its own events.

local ecs = require("ecs")

local fill = assert(ecs.service.get("ui.fill", "void(handle<ecs.surface>, float, float, float, float, int64)"))
local text = assert(ecs.service.get("ui.text", "float(handle<ecs.surface>, string, float, float, float, int64)"))
local color = assert(ecs.service.get("ui.color", "int64(string)"))

local CHOICES = { { "Red", "#BF616A" }, { "Green", "#A3BE8C" }, { "Blue", "#5E81AC" }, { "Yellow", "#EBCB8B" } }
local ROW, WIDTH = 28, 160 -- in layout units

-- the menu's row under a position of its surface, or nil
local function rowAt(y, scale)
  local row = math.floor(y / scale / ROW) + 1
  return CHOICES[row] and row or nil
end

-- opens the menu at a point of the panel; at most one is open
local function openMenu(picker, x, y)
  if picker.menu then
    picker.menu:close()
  end

  local hover = nil
  local menu

  -- the anchor is a rectangle of the panel, in its surface pixels; the menu opens below it, or above it near the bottom
  menu = picker.panel:openPopup({
    kind = "menu", -- a menu takes the keys while it is open, and Escape closes it
    anchor = { x = x, y = y, width = 0, height = 0 },
    width = WIDTH,
    height = ROW * #CHOICES,

    -- the surface is the popup's own; what draw leaves out is transparent
    draw = function(surface)
      fill(surface, 0, 0, WIDTH, ROW * #CHOICES, color("background"))

      for i, choice in ipairs(CHOICES) do
        if i == hover then
          fill(surface, 0, (i - 1) * ROW, WIDTH, ROW, color("selected"))
        end

        fill(surface, 8, (i - 1) * ROW + 8, 12, 12, color(choice[2]))
        text(surface, choice[1], 28, (i - 1) * ROW + 5, 15, color("text"))
      end
    end,

    -- positions are in the popup's surface pixels
    event = function(event)
      if event.type == "pointerMove" then
        hover = rowAt(event.y, picker.scale)
        menu:redraw()
      elseif event.type == "pointerUp" and rowAt(event.y, picker.scale) then
        picker.color = CHOICES[rowAt(event.y, picker.scale)][2]
        picker.panel:redraw()
        menu:close() -- closed runs after this callback returns
      end
    end,

    closed = function()
      picker.menu = nil
    end,
  })

  picker.menu = menu
end

ecs.panel.registerType({
  name = "picker.panel",
  title = "Picker",
  stateVersion = 1,

  create = function(panel, saved)
    return { panel = panel, color = saved and saved.color or "#4C566A", scale = 1 }
  end,

  draw = function(picker, surface)
    picker.scale = surface.scale
    fill(surface, 0, 0, surface.width / surface.scale, surface.height / surface.scale, color(picker.color))
    text(surface, "Right-click to choose a colour", 24, 24, 18, color("text"))
  end,

  event = function(picker, event)
    if event.type == "pointerDown" and event.button == 3 then
      openMenu(picker, event.x, event.y)
    end
  end,

  saveState = function(picker)
    return { color = picker.color }
  end,
})
