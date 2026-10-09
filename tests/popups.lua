-- popups: a menu opens below its anchor, gets the pointer and the keys, and closes on Escape and on a press outside it; a tooltip gets no keys, and closes from code and with its panel
return {
  preset = "presets/popper.lua",
  run = function(test)
    local panel = test.rect(1)

    -- a right click opens a menu below the point; the offscreen driver has no popup windows, so it is drawn inside the window
    test.click(panel.x + 100, panel.y + 50, 3)
    test.match(test.popup(1), { window = 1, x = panel.x + 100, y = panel.y + 60, width = 120, height = 80 }, "the menu")

    -- the menu gets a click in its own positions, and the keys
    test.click(panel.x + 110, panel.y + 70)
    test.key("Down")
    test.key("Escape")
    local state = test.session().pluginState.popper.state
    test.match(state.events, {
      { kind = "menu", type = "pointerDown", x = 10, y = 10 },
      { kind = "menu", type = "pointerUp", x = 10, y = 10 },
      { kind = "menu", type = "keyDown", key = "Down" },
      { kind = "menu", type = "keyUp", key = "Down" },
    }, "the menu's events")
    test.match(state.closed, { "menu" }, "Escape closes the menu")
    assert(not pcall(test.popup, 1), "no popup is open")

    -- near the bottom, a menu opens above its anchor; a press outside closes it and does nothing else
    test.click(panel.x + 100, panel.y + panel.height - 20, 3)
    test.match(test.popup(1), { y = panel.y + panel.height - 20 - 80 }, "the menu above its anchor")
    test.click(panel.x + 400, panel.y + 300)
    test.match(test.session().pluginState.popper.state.closed, { "menu", "menu" }, "a press outside closes the menu")

    -- a tooltip takes no keys; its panel's key closes it
    test.key("T")
    test.match(test.popup(1), { x = panel.x + 20, y = panel.y + 30 }, "the tooltip")
    test.key("C")
    test.match(test.session().pluginState.popper.state.closed, { "menu", "menu", "tooltip" }, "the tooltip closed from code")

    -- closing the panel closes its popups
    test.key("T")
    test.call("ecs.layout.close")
    test.match(test.session().pluginState.popper.state.closed, { "menu", "menu", "tooltip", "tooltip" }, "closing the panel")
  end,
}
