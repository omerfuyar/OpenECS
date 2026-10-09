-- a panel that opens a menu where it is clicked and a tooltip with a key, and keeps what its popups saw in its plugin state

local ecs = require("ecs")

local results = { events = {}, closed = {} }

-- fills a surface with one colour
local function fill(surface, color)
  local row = string.pack("=I4", color):rep(surface.width)

  for y = 0, surface.height - 1 do
    surface:setRow(y, row)
  end
end

local function open(panel, kind, x, y)
  local popup, message = panel:openPopup({
    kind = kind,
    anchor = { x = x, y = y, width = 10, height = 10 },
    width = 120,
    height = 80,
    draw = function(surface)
      fill(surface, kind == "menu" and 0xFFBF616A or 0xFFEBCB8B)
    end,
    event = function(event)
      local seen = { kind = kind, type = event.type, key = event.key }

      if event.x then
        seen.x, seen.y = math.floor(event.x), math.floor(event.y)
      end

      table.insert(results.events, seen)
    end,
    closed = function()
      table.insert(results.closed, kind)
    end,
  })

  results.opened = popup ~= nil or message
  return popup
end

ecs.panel.registerType({
  name = "popper.panel",
  title = "Popper",
  create = function(panel)
    return { panel = panel }
  end,
  draw = function(_, surface)
    fill(surface, 0xFF3B4252)
  end,
  event = function(popper, event)
    if event.type == "pointerDown" and event.button == 3 then
      open(popper.panel, "menu", event.x, event.y)
    elseif event.type == "keyDown" and event.key == "T" then
      -- a tooltip closes itself with the same key
      popper.tooltip = open(popper.panel, "tooltip", 20, 20)
    elseif event.type == "keyDown" and event.key == "C" and popper.tooltip then
      popper.tooltip:close()
      popper.tooltip = nil
    end
  end,
})

ecs.plugin.registerState({
  version = 1,
  save = function()
    return results
  end,
  restore = function() end,
})
