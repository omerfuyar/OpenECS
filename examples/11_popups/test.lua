-- a right click opens the menu at the pointer, and a click on a row chooses its colour and closes the menu
return {
  preset = "preset.lua",
  run = function(test)
    local panel = test.rect(1)
    test.click(panel.x + 200, panel.y + 100, 3)

    local menu = test.popup(1)
    test.click(menu.x + 20, menu.y + 28 * 2 + 10) -- the third row
    test.match(test.session().workspaces[1].windows[1].panels[1].state, { color = "#5E81AC" })
    assert(not pcall(test.popup, 1), "the menu has closed")
  end,
}
