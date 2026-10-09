-- bound keys step the focused stepper, Space comes to the panel itself, and the preset's keys add and reset
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

    test.key("Shift+Up")
    test.match(test.panel(2), { title = "Stepper (11)" }, "after the preset's Shift+Up for the panel type")

    test.key("Ctrl+0")
    test.match(test.panel(1), { title = "Stepper (0)" }, "after Ctrl+0")
    test.match(test.panel(2), { title = "Stepper (0)" }, "after Ctrl+0")

    test.key("Up")
    test.key("Alt+W")
    test.key("Z")
    test.match(test.panel(2), { title = "Stepper (0)" }, "after the preset's key after the prefix")
  end,
}
