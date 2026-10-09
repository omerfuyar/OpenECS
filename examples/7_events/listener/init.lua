-- Panels that show the ticks of the ticker plugin, and how many panels opened. Events connect plugins that do not call each other.

local ecs = require("ecs")

local listeners = {} -- every open listener, by its panel
local opened = 0

local function show(listener)
  listener.panel:setTitle(("Ticks %d, panels %d"):format(listener.ticks, opened))
end

-- subscribe after the plugin that declares the event has loaded; the subscription lasts until it is cancelled
ecs.event.subscribe("ticker.tick", function(_, ticks)
  for _, listener in pairs(listeners) do
    listener.ticks = ticks
    show(listener)
  end
end)

-- the core's events start with "ecs."; ecs.panelOpened's value names the panel and its type
ecs.event.subscribe("ecs.panelOpened", function(_, value)
  opened = opened + 1
  ecs.log.debug(("A %s opened."):format(value.type))
end)

ecs.panel.registerType({
  name = "listener.panel",
  title = "Listener",

  create = function(panel)
    local listener = { panel = panel, ticks = 0 }
    listeners[panel] = listener
    return listener
  end,

  destroy = function(listener)
    listeners[listener.panel] = nil
  end,

  draw = function(_, surface)
    local row = string.pack("=I4", 0xFF3B4252):rep(surface.width)

    for y = 0, surface.height - 1 do
      surface:setRow(y, row)
    end
  end,
})
