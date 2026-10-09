-- the keys after the core prefix move focus, maximize, close and reopen
return {
  preset = "presets/boxes.lua",
  run = function(test)
    local function workspace()
      return test.session().workspaces[1]
    end

    test.match(workspace().focus, 1, "the first box has the focus at start")
    test.key("Alt+W")
    test.key("Right")
    test.match(workspace().focus, 2, "focus moves to the second box")

    test.key("Alt+W")
    test.key("M")
    test.match(workspace().windows[1][2].maximized, true, "the second box is maximized")
    test.key("Alt+W")
    test.key("M")
    test.match(workspace().windows[1][2].maximized, nil, "the second box is restored")

    test.key("Alt+W")
    test.key("X")
    test.match(workspace().windows[1], { panels = { { type = "boxes.one" } } }, "the second box is closed")

    test.key("Alt+W")
    test.key("T")
    test.match(workspace().windows[1], { panels = { { type = "boxes.one" }, { type = "boxes.two" } } }, "the second box is reopened in the focused group")
  end,
}
