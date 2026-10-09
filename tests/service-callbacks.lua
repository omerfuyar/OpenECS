-- Lua functions given as fn<...> arguments are called by a C service and by a Lua service
return {
  preset = "presets/callbacks.lua",
  run = function(test)
    local box = test.rect(1)
    test.click(box.x + 50, box.y + 60)

    test.call("callbacks.run")
    local results = test.session().pluginState.callbacks.state
    test.match(results.c, { count = 1, seen = { { type = "cboxes.box", clicks = 0 } }, without = 1 }, "cboxes")
    test.match(results.lua, { count = 1, seen = { { type = "boxes.one", clicks = 1 } }, without = 1 }, "boxes")
    test.match(results.refused, { true, true, true, true }, "bad signatures")
  end,
}
