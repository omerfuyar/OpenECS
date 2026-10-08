-- ecs.session.save writes the session that quitting would save
return {
  preset = "presets/saver.lua",
  run = function(test)
    local canvas = test.rect(1)
    test.drag(canvas.x + 50, canvas.y + 60, canvas.x + 200, canvas.y + 150)
    test.call("ecs.workspace2")

    test.call("saver.save")
    local session = test.session()
    local results = session.pluginState.saver.state
    test.match(results, { saved = true }, "saving")

    local saved = dofile(results.path)
    os.remove(results.path)

    -- the file is written before saving gives its result
    results.saved = nil
    test.match(saved, session, "the saved file")
    test.match(saved, { currentWorkspace = 2, workspaces = { { windows = { { panels = { { state = { strokes = { {} } } } } } } }, {} } }, "what it holds")
  end,
}
