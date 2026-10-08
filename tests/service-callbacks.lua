-- Lua functions given as fn<...> arguments are called by a C service and by a Lua service
return {
  preset = "presets/callbacks.lua",
  run = function(test)
    local c = test.rect(1)
    test.drag(c.x + 50, c.y + 60, c.x + 200, c.y + 150)

    test.call("callbacks.run")
    local results = test.session().pluginState.callbacks.state
    test.match(results.c, { count = 1, seen = { { type = "sketch_c.canvas", strokes = 1 } }, without = 1 }, "sketch_c")
    test.match(results.lua, { count = 1, seen = { { type = "sketch_lua.canvas", strokes = 0 } }, without = 1 }, "sketch_lua")
    test.match(results.refused, { true, true, true, true }, "bad signatures")
  end,
}
