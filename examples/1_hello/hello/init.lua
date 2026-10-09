-- The smallest plugin: one panel type that fills itself with one colour.
-- A panel type is a kind of panel; each panel of the type is made from it, and the core arranges the panels.

local ecs = require("ecs") -- each plugin gets its own ecs table, the plugin interface; there is no global ecs

ecs.log.info("Hello from Lua!") -- this file runs once, when the plugin loads; the line goes to the terminal and the log file

-- register panel types while the plugin loads, not later
ecs.panel.registerType({
  name = "hello.greeting", -- the type's name: the plugin's name, a dot, and a name of its own
  title = "Greeting",      -- what the panel's tab shows

  -- runs for each new panel; what it returns is the panel's state, which the other functions get
  create = function(panel)
    return { panel = panel }
  end,

  -- runs when the panel needs drawing; the surface is the panel's pixels, valid only during this call
  draw = function(_, surface)
    -- a colour is ARGB: alpha, red, green and blue, one byte each, so 0xFF5E81AC is an opaque blue
    local row = string.pack("=I4", 0xFF5E81AC):rep(surface.width) -- one row of pixels, four bytes each

    for y = 0, surface.height - 1 do
      surface:setRow(y, row)
    end
  end,
})
