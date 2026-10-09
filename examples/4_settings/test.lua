-- the preset's value is in effect, and the wheel changes the size
return {
  preset = "preset.lua",
  run = function(test)
    test.match(test.panel(1), { title = "Hello, OpenECS (32)" })

    local panel = test.rect(1)
    test.wheel(panel.x + 50, panel.y + 50, 1)
    test.match(test.panel(1), { title = "Hello, OpenECS (34)" }, "after the wheel")
  end,
}
