-- the group's menu and the panel's menu run their entries
return {
  preset = "presets/sketch.lua",
  run = function(test)
    test.call("ecs.workspace3")
    local clocks = test.session().workspaces[3].windows[1][2].panels
    local clock = test.rect(clocks[1].id)
    local tabRow = clock.y - 13

    -- the group's menu: Tabs, then Maximize the group
    test.click(clock.x + clock.width - 20, tabRow, 3)
    test.key("Down")
    test.key("Return")
    test.match(test.session().workspaces[3].windows[1][2].maximized, true, "the group's menu maximizes")
    test.call("ecs.maximize")

    -- the panel's menu: its first entry closes the panel
    test.click(clock.x + 40, tabRow, 3)
    test.key("Return")
    test.match(test.session().workspaces[3].windows[1][2], { panels = { { type = "sketch_lua.clock" } } }, "the panel's menu closes")
  end,
}
