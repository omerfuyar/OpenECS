-- dragging a grip onto the centre of a panel groups them; dragging a tab to an edge splits them again
return {
  preset = "presets/boxes.lua",
  run = function(test)
    local one = test.rect(1)
    local two = test.rect(2)

    -- the grip shows near the top edge of a panel without a tab row
    test.move(two.x + two.width / 2, two.y + 30)
    test.drag(two.x + two.width / 2, two.y + 8, one.x + one.width / 2, one.y + one.height / 2)
    test.match(test.session().workspaces[1].windows[1], { panels = { { type = "boxes.one" }, { type = "boxes.two" } } }, "grouped")

    -- the second box is shown, and its tab is the second one in the tab row
    two = test.rect(2)
    test.drag(two.x + 150, two.y - 13, two.x + two.width - 10, two.y + two.height / 2)
    test.match(test.session().workspaces[1].windows[1], { split = "horizontal", { panels = { { type = "boxes.one" } } }, { panels = { { type = "boxes.two" } } } }, "split again")
  end,
}
