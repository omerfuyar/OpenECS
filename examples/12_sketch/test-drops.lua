-- both sketch plugins drag their strokes with Shift, and take dropped strokes, text and files

-- the folder of this test, for the file it drops
local folder = debug.getinfo(1, "S").source:match("^@(.*/)") or "./"

return {
  preset = "preset.lua",
  run = function(test)
    test.call("ecs.workspace.switch3")
    local canvases = test.session().workspaces[3].windows[1][1]
    local c = test.rect(canvases[1].panels[1].id)
    local lua = test.rect(canvases[2].panels[1].id)

    -- a stroke on the C canvas, dragged to the Lua canvas and back
    test.drag(c.x + 50, c.y + 60, c.x + 200, c.y + 150)
    test.drag(c.x + 100, c.y + 100, lua.x + 100, lua.y + 100, "Shift")
    test.drag(lua.x + 100, lua.y + 100, c.x + 100, c.y + 100, "Shift")

    -- a file of three strokes on the C canvas, and a stroke as text on the Lua canvas
    test.dropFiles(c.x + 10, c.y + 10, { folder .. "three-strokes.txt" })
    test.dropText(lua.x + 10, lua.y + 10, "4 4278190335 10.0 10.0 20.0 20.0\n")

    canvases = test.session().workspaces[3].windows[1][1]
    local cStrokes = canvases[1].panels[1].state.strokes
    local luaStrokes = canvases[2].panels[1].state.strokes
    test.match(#cStrokes, 5, "strokes on the C canvas: drawn, dragged back and from the file")
    test.match(#luaStrokes, 2, "strokes on the Lua canvas: dragged and dropped as text")
    test.match(luaStrokes[1], { size = cStrokes[1].size, color = cStrokes[1].color }, "the dragged stroke")
    test.match(cStrokes[2], { size = cStrokes[1].size, color = cStrokes[1].color }, "the stroke dragged back")
    test.match(cStrokes[3], { size = 4, points = { 10, 10, 50, 50 } }, "the first stroke of the file")
    test.match(luaStrokes[2], { size = 4, color = 4278190335, points = { 10, 10, 20, 20 } }, "the stroke dropped as text")
  end,
}
