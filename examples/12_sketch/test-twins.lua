-- the same stroke on the C canvas and on the Lua canvas saves the same points
return {
  preset = "preset.lua",
  run = function(test)
    test.call("ecs.workspace.switch3")
    local canvases = test.session().workspaces[3].windows[1][1]
    local c = test.rect(canvases[1].panels[1].id)
    local lua = test.rect(canvases[2].panels[1].id)

    test.drag(c.x + 50, c.y + 60, c.x + 200, c.y + 150)
    test.drag(lua.x + 50, lua.y + 60, lua.x + 200, lua.y + 150)

    canvases = test.session().workspaces[3].windows[1][1]
    local cStrokes = canvases[1].panels[1].state.strokes
    local luaStrokes = canvases[2].panels[1].state.strokes
    test.match(#cStrokes, 1, "strokes on the C canvas")
    test.match(luaStrokes, { { points = cStrokes[1].points, size = cStrokes[1].size } })
  end,
}
