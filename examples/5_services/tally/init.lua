-- Panels that count clicks with the counter plugin's functions. Every panel adds to the same counter, so each shows the total.
-- It also offers a function of its own, tally.reset.

local ecs = require("ecs")

local fill = assert(ecs.service.get("ui.fill", "void(handle<ecs.surface>, float, float, float, float, int64)"))
local text = assert(ecs.service.get("ui.text", "float(handle<ecs.surface>, string, float, float, float, int64)"))
local color = assert(ecs.service.get("ui.color", "int64(string)"))

-- a C function, called from Lua like any other; get fails if the signature differs from the registered one
local add = assert(ecs.service.get("counter.add", "int(string, int)"))
local get = assert(ecs.service.get("counter.get", "int(string)"))

local panels = {}

local function redrawAll()
  for panel in pairs(panels) do
    panel:redraw()
  end
end

ecs.panel.registerType({
  name = "tally.panel",
  title = "Tally",

  create = function(panel)
    panels[panel] = true
    return { panel = panel }
  end,

  destroy = function(tally)
    panels[tally.panel] = nil
  end,

  draw = function(_, surface)
    fill(surface, 0, 0, surface.width / surface.scale, surface.height / surface.scale, color("background"))
    text(surface, ("%d clicks in all"):format(get("clicks")), 24, 24, 24, color("text"))
  end,

  event = function(tally, event)
    if event.type == "pointerDown" then
      tally.panel:setTitle(("Tally (%d)"):format(add("clicks", 1)))
      redrawAll()
    end
  end,
})

-- register functions while the plugin loads; keys, menus and other plugins run them by name
assert(ecs.service.register("tally", {
  reset = {
    sig = "void()",
    doc = "Sets the click counter back to 0",
    fn = function()
      add("clicks", -get("clicks"))
      redrawAll()
    end,
  },
}))
