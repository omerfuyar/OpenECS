-- bound keys step the focused stepper, Space comes to the panel itself, and the preset's key resets every stepper
return {
  preset = "preset.lua",
  run = function(test)
    test.key("Up")
    test.key("Up")
    test.key("Down")
    test.match(test.panel(1), { title = "Stepper (1)" }, "after Up, Up and Down")

    local right = test.rect(2)
    test.click(right.x + 20, right.y + 20) -- a click gives the panel the focus
    test.key("Space")
    test.match(test.panel(2), { title = "Stepper (10)" }, "after Space")

    test.key("Ctrl+0")
    test.match(test.panel(1), { title = "Stepper (0)" }, "after Ctrl+0")
    test.match(test.panel(2), { title = "Stepper (0)" }, "after Ctrl+0")
  end,
}
