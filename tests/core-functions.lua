-- every function that keys can run is a function of the ecs module with the same name, which acts on the focused panel
return {
  preset = "presets/actions.lua",
  run = function(test)
    test.call("actions.run")
    local session = test.session()

    test.match(session.pluginState.actions.state, {
      types = { maximize = "function", restart = "function", workspace1 = "function", moveToWorkspace10 = "function" },
      closedLocked = false,
    }, "the module")

    local first = session.workspaces[1].windows[1]
    test.match(first, { { panels = { {} } }, { panels = { {} } } }, "the locked group kept its panel")
    test.match(session, { currentWorkspace = 2 }, "switching")
    test.match(first[1].maximized or first[2].maximized, true, "maximizing")
  end,
}
