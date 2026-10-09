-- Emits ticker.tick ten times a second. It has no panels: a plugin can only offer functions, events or settings.

local ecs = require("ecs")

-- an event is a named message with a value; any plugin that depends on this one can subscribe to it
-- a plugin emits only the events it declared, while it loads
ecs.event.declare("ticker.tick", "Ten times a second: the number of ticks so far")

local ticks = 0

-- a timer calls a function after some seconds, once or again and again; all Lua code runs on the program's main thread
-- a plugin timer runs until it is stopped; a panel's timer (panel:startTimer) stops when its panel closes
local timer = ecs.timer.start(0.1, true, function()
  ticks = ticks + 1
  ecs.event.emit("ticker.tick", ticks) -- subscribers get it after this function returns, not during the call
end)

-- runs once, before the program exits
ecs.plugin.onShutdown(function()
  timer:stop()
end)
