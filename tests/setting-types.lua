-- a value that does not have its setting's type is skipped, so the default is in effect: a colour, a key combination and a choice
return {
  preset = "presets/bad-settings.lua",
  run = function(test)
    local state = test.session().pluginState.probe.state
    test.match(state.accent, "#4C8BF5", "ecs.colorAccent")
    test.match(state.focus, "click", "ecs.focus")

    -- the prefix is still Alt+W, so X after it closes the box
    test.key("Alt+W")
    test.key("X")
    test.match(test.session().workspaces[1].windows[1], nil, "the windows after Alt+W and X")
  end,
}
