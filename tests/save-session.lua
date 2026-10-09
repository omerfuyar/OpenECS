-- ecs.session.save writes the session that quitting would save, and a test cannot open a session
return {
  preset = "presets/saver.lua",
  run = function(test)
    local box = test.rect(1)
    test.click(box.x + 50, box.y + 60)
    test.call("ecs.workspace.switch2")

    test.call("saver.save")
    local session = test.session()
    local results = session.pluginState.saver.state
    test.match(results, { saved = true, opened = false }, "saving and opening")

    local saved = dofile(results.path)
    os.remove(results.path)

    -- the file is written before saving and opening give their results
    results.saved = nil
    results.opened = nil
    test.match(saved, session, "the saved file")
    test.match(saved, { currentWorkspace = 2, workspaces = { { windows = { { panels = { { state = { clicks = 1 } } } } } }, {} } }, "what it holds")
  end,
}
