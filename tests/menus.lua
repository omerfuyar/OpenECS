-- the group's menu and the panel's menu run their entries
return {
  preset = "presets/boxes.lua",
  run = function(test)
    test.call("ecs.workspace.switch3")
    local group = test.session().workspaces[3].windows[1][2].panels
    local box = test.rect(group[1].id)
    local tabRow = box.y - 13

    -- the group's menu: Tabs, then Maximize the group
    test.click(box.x + box.width - 20, tabRow, 3)
    test.key("Down")
    test.key("Return")
    test.match(test.session().workspaces[3].windows[1][2].maximized, true, "the group's menu maximizes")
    test.call("ecs.layout.maximize")

    -- the panel's menu: its first entry closes the panel
    test.click(box.x + 40, tabRow, 3)
    test.key("Return")
    test.match(test.session().workspaces[3].windows[1][2], { panels = { { type = "boxes.three" } } }, "the panel's menu closes")
  end,
}
