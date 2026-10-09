-- A greeting whose name, colour and size are settings. The settings window (Alt+W, then ",") shows and changes them.

local ecs = require("ecs")

local fill = assert(ecs.service.get("ui.fill", "void(handle<ecs.surface>, float, float, float, float, int64)"))
local text = assert(ecs.service.get("ui.text", "float(handle<ecs.surface>, string, float, float, float, int64)"))
local color = assert(ecs.service.get("ui.color", "int64(string)"))

local COLORS = { blue = "#5E81AC", green = "#A3BE8C", red = "#BF616A" }

local greetings = {} -- every open greeting, by its panel

local function title()
  return ("Hello, %s (%d)"):format(ecs.settings.get("greeter.name"), ecs.settings.get("greeter.size")) -- get gives the value of the highest layer that sets it
end

-- a setting's changed function runs after the value in effect changes, from any layer
local function changed()
  for _, greeting in pairs(greetings) do
    greeting.panel:setTitle(title())
    greeting.panel:redraw()
  end
end

-- declare settings while the plugin loads; a setting's name starts with the plugin's name
-- the type says what values it takes: bool, integer, number, string, choice (one of choices), key, list or table
-- the value in effect comes from the highest layer that sets it: the default, the preset, the settings window, then the user's file
ecs.settings.declare({ name = "greeter.name", type = "string", description = "Who to greet", default = "world", changed = changed })
ecs.settings.declare({ name = "greeter.color", type = "choice", description = "Colour of the greeting", default = "blue", choices = { "blue", "green", "red" }, changed = changed })
ecs.settings.declare({ name = "greeter.size", type = "integer", description = "Size of the greeting; the wheel changes it", default = 32, changed = changed })

ecs.panel.registerType({
  name = "greeter.panel",
  title = "Greeting",

  create = function(panel)
    local greeting = { panel = panel }
    greetings[panel] = greeting
    panel:setTitle(title())
    return greeting
  end,

  destroy = function(greeting)
    greetings[greeting.panel] = nil
  end,

  draw = function(_, surface)
    local width, height = surface.width / surface.scale, surface.height / surface.scale
    fill(surface, 0, 0, width, height, color("background"))
    text(surface, "Hello, " .. ecs.settings.get("greeter.name") .. "!", 24, 24, ecs.settings.get("greeter.size"), color(COLORS[ecs.settings.get("greeter.color")]))
  end,

  event = function(_, event)
    if event.type == "wheel" then
      -- set writes the user's settings file; changed runs afterwards, not during this call
      local size = ecs.settings.get("greeter.size") + (event.wheelY > 0 and 2 or -2)
      ecs.settings.set("greeter.size", math.max(8, math.min(96, size)))
    elseif event.type == "pointerDown" then
      -- explain tells where a value comes from: the default, the preset, the settings window or the user's file
      local explanation = assert(ecs.settings.explain("greeter.name"))
      ecs.log.info(("greeter.name is '%s', from the %s layer."):format(explanation.value, explanation.layer))
    end
  end,
})
