-- while the focused panel takes text input, keys without Ctrl, Alt or Super go to it instead of the bindings; others still run bindings
return {
  preset = "presets/typist.lua",
  run = function(test)
    local function results()
      return test.session().pluginState.typist.state
    end

    -- A is bound, but the pad types it
    test.key("A")
    test.text("a")
    test.match(results(), { bound = 0, text = "a", keys = { "A" } }, "A while typing")

    -- Ctrl+B still runs its binding
    test.key("Ctrl+B")
    test.match(results(), { ctrlBound = 1, keys = { "A" } }, "Ctrl+B while typing")

    -- without text input, A runs its binding
    test.call("typist.stop")
    test.key("A")
    test.match(results(), { bound = 1, keys = { "A" } }, "A without text input")
  end,
}
