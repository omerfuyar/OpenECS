-- code opens panels in pop-out windows with the zone "window", moves them out, and back into the main window
return {
  preset = "presets/windows.lua",
  run = function(test)
    test.call("windows.run")
    local session = test.session()
    test.match(session.pluginState.windows.state, { opened = true, moved = true, refused = true, stayed = true }, "the calls")
    test.match(session.workspaces[1].windows, {
      { panels = { { type = "boxes.two" } } },
      { panels = { { type = "boxes.three" } } },
      { panels = { { type = "boxes.one" } } },
    }, "the windows")
    test.match(test.rect(1).window, 3, "the moved panel's window")

    -- back in the main window, its pop-out window is gone
    test.call("windows.back")
    test.match(test.session().workspaces[1].windows, {
      { split = "horizontal", { panels = { { type = "boxes.two" } } }, { panels = { { type = "boxes.one" } } } },
      { panels = { { type = "boxes.three" } } },
    }, "after moving back")
    test.match(test.rect(1).window, 1, "the panel back in the main window")
  end,
}
