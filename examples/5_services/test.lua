-- both panels add to the same counter in C, and tally.reset sets it back
return {
  preset = "preset.lua",
  run = function(test)
    local left, right = test.rect(1), test.rect(2)
    test.click(left.x + 20, left.y + 20)
    test.click(right.x + 20, right.y + 20)
    test.match(test.panel(2), { title = "Tally (2)" })

    test.call("tally.reset")
    test.click(left.x + 20, left.y + 20)
    test.match(test.panel(1), { title = "Tally (1)" })
  end,
}
