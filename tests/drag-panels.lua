-- dragging a grip onto the centre of a panel groups them; dragging a tab to an edge splits them again
return {
  preset = "presets/sketch.lua",
  run = function(test)
    local canvas = test.rect(1)
    local clock = test.rect(2)

    -- the grip shows near the top edge of a panel without a tab row
    test.move(clock.x + clock.width / 2, clock.y + 30)
    test.drag(clock.x + clock.width / 2, clock.y + 8, canvas.x + canvas.width / 2, canvas.y + canvas.height / 2)
    test.match(test.session().workspaces[1].windows[1], { panels = { { type = "sketch_c.canvas" }, { type = "sketch_c.clock" } } }, "grouped")

    -- the clock is shown, and its tab is the second one in the tab row
    clock = test.rect(2)
    test.drag(clock.x + 150, clock.y - 13, clock.x + clock.width - 10, clock.y + clock.height / 2)
    test.match(test.session().workspaces[1].windows[1], { split = "horizontal", { panels = { { type = "sketch_c.canvas" } } }, { panels = { { type = "sketch_c.clock" } } } }, "split again")
  end,
}
