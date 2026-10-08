-- the keys after the core prefix move focus, maximize, close and reopen
return {
  run = function(test)
    local function workspace()
      return test.session().workspaces[1]
    end

    test.match(workspace().focus, 1, "the canvas has the focus at start")
    test.key("Alt+W")
    test.key("Right")
    test.match(workspace().focus, 2, "focus moves to the clock")

    test.key("Alt+W")
    test.key("M")
    test.match(workspace().windows[1][2].maximized, true, "the clock is maximized")
    test.key("Alt+W")
    test.key("M")
    test.match(workspace().windows[1][2].maximized, nil, "the clock is restored")

    test.key("Alt+W")
    test.key("X")
    test.match(workspace().windows[1], { panels = { { type = "sketch_c.canvas" } } }, "the clock is closed")

    test.key("Alt+W")
    test.key("T")
    test.match(workspace().windows[1], { panels = { { type = "sketch_c.canvas" }, { type = "sketch_c.clock" } } }, "the clock is reopened in the focused group")
  end,
}
