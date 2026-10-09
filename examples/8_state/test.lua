-- clicks add dots to the panel's saved state and mark it unsaved; the second panel starts from the preset's state
return {
  preset = "preset.lua",
  run = function(test)
    test.match(test.panel(1).unsaved, false, "a new panel")

    local left = test.rect(1)
    test.click(left.x + 10, left.y + 20)
    test.click(left.x + 30, left.y + 40)

    local session = test.session()
    local panels = session.workspaces[1].windows[1]
    test.match(panels[1].panels[1].state, { dots = { 10, 20, 30, 40 } }, "the clicked panel")
    test.match(panels[2].panels[1].state, { dots = { 40, 40, 80, 80 } }, "the panel the preset gave a state")
    test.match(session.pluginState.marks.state, { placed = 2 }, "the plugin's state")
    test.match(test.panel(1).unsaved, true, "after the clicks")
  end,
}
