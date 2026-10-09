-- Emits ticker.tick ten times a second. It has no panels: a plugin can only offer functions, events or settings.

local ecs = require("ecs")

-- a plugin emits only the events it declared, while it loads
ecs.event.declare("ticker.tick", "Ten times a second: the number of ticks so far")

local ticks = 0

-- a plugin timer runs on the main thread until it is stopped; a panel's timer (panel:startTimer) stops when its panel closes
local timer = ecs.timer.start(0.1, true, function()
  ticks = ticks + 1
  ecs.event.emit("ticker.tick", ticks) -- subscribers get it after this function returns, not during the call
end)

-- runs once, before the program exits
ecs.plugin.onShutdown(function()
  timer:stop()
end)
