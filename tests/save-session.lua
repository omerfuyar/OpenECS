-- ecs.session.save writes the session that quitting would save, and a test cannot open a session
return {
  preset = "presets/saver.lua",
  run = function(test)
    local canvas = test.rect(1)
    test.drag(canvas.x + 50, canvas.y + 60, canvas.x + 200, canvas.y + 150)
    test.call("ecs.workspace2")

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
    test.match(saved, { currentWorkspace = 2, workspaces = { { windows = { { panels = { { state = { strokes = { {} } } } } } } }, {} } }, "what it holds")
  end,
}
