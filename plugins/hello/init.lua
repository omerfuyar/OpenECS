-- Example Lua plugin: it uses the ecs table that the core gives it.

ecs.log.info("Hello from " .. ecs.plugin.name .. " " .. ecs.plugin.version .. ".")

ecs.settings.declare({
  name = "hello.greetings",
  type = "integer",
  description = "How many greetings the hello plugin logs",
  default = 2,
})

-- a repeating timer that stops itself after a few calls
local count = 0
local timer
timer = ecs.timer.start(0.5, true, function()
  count = count + 1
  ecs.log.info("Greeting " .. count .. ".")

  if count >= ecs.settings.get("hello.greetings") then
    timer:stop()
  end
end)
