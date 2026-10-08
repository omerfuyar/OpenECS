-- a preset's ecs.prefixKeys adds to the keys of the core's settings file, and false removes one
return {
  preset = "presets/changed-keys.lua",
  run = function(test)
    test.key("Alt+W")
    test.key("X")
    test.match(#test.session().workspaces[1].windows[1], 2, "X is removed")

    test.key("Alt+W")
    test.key("M")
    test.match(test.session().workspaces[1].windows[1][1].maximized, true, "M is kept from the core's settings file")
    test.call("ecs.layout.maximize")

    test.key("Alt+W")
    test.key("K")
    test.match(test.session().workspaces[1].windows[1], { panels = { { type = "sketch_c.clock" } } }, "K closes the focused canvas")
  end,
}
