-- the listener hears the ticker's ticks, and counts its own panel opening
return {
  preset = "preset.lua",
  run = function(test)
    test.wait(0.5) -- lets the program run, so the timer ticks

    local ticks, panels = test.panel(1).title:match("^Ticks (%d+), panels (%d+)$")
    assert(tonumber(ticks) >= 2, "ticks heard: " .. tostring(ticks))
    test.match(panels, "1", "panels opened")
  end,
}
