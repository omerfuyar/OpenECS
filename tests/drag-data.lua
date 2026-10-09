-- a panel drags data to the panels that accept its type, and files and text from other applications drop the same way

local function centre(id)
  local rect = test.rect(id)
  return rect.x + rect.width / 2, rect.y + rect.height / 2
end

return {
  preset = "presets/dropper.lua",
  run = function(test)
    _G.test = test
    local ax, ay = centre(1)
    local bx, by = centre(2)
    local cx, cy = centre(3)

    -- the colour lands on the second box, which accepts it, then on the third, which does not
    test.drag(ax, ay, bx, by)
    test.drag(ax, ay, cx, cy)

    -- Escape cancels a drag
    test.press(ax, ay)
    test.move(bx, by)
    test.key("Escape")
    test.release(bx, by)

    -- files and text from another application; the second box takes no text
    test.dropFiles(ax, ay, { "/tmp/one.png", "/tmp/two.png" })
    test.dropText(bx, by, "ignored")
    test.dropText(ax, ay, "hello")

    -- a drag needs a pressed button; the first box has the focus since it was pressed
    test.call("dropper.tryDrag")

    local results = test.session().pluginState.dropper.state
    test.match(results, {
      started = true,
      releases = 3,
      refused = true,
      drops = {
        { panel = 2, dataType = "color", value = { name = "red", argb = 0xFFFF0000 }, inside = true },
        { panel = 1, dataType = "file-list", value = { "/tmp/one.png", "/tmp/two.png" }, inside = true },
        { panel = 1, dataType = "text", value = "hello", inside = true },
      },
    }, "what the boxes got")
  end,
}
