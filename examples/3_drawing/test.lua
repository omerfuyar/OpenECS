-- clicks on the button count, and clicks beside it do not
return {
  preset = "preset.lua",
  run = function(test)
    local panel = test.rect(1)
    local x, y = panel.x + panel.width / 2, panel.y + panel.height / 2

    test.click(x, y)
    test.click(x, y)
    test.click(panel.x + 5, panel.y + 5)
    test.match(test.panel(1), { title = "Button (2)" })
  end,
}
