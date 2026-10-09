-- The smallest plugin: one panel type that fills itself with one colour.

local ecs = require("ecs") -- each plugin gets its own ecs table; there is no global ecs

ecs.log.info("Hello from Lua!") -- this file runs once, when the plugin loads

-- register panel types while the plugin loads, not later
ecs.panel.registerType({
  name = "hello.greeting",
  title = "Greeting",

  -- runs for each new panel; what it returns is the panel's state, which the other functions get
  create = function(panel)
    return { panel = panel }
  end,

  -- runs when the panel needs drawing; the surface is valid only during this call
  draw = function(_, surface)
    local row = string.pack("=I4", 0xFF5E81AC):rep(surface.width) -- one row of ARGB pixels

    for y = 0, surface.height - 1 do
      surface:setRow(y, row)
    end
  end,
})
