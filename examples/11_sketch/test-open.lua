-- files on the command line go to the preset's open function, here sketch_c.open, which opens a canvas with each file's strokes

-- the panels of a layout node and its children
local function addPanels(node, panels)
  for _, panel in ipairs(node.panels or {}) do
    panels[#panels + 1] = panel
  end

  for _, child in ipairs(node) do
    addPanels(child, panels)
  end
end

return {
  preset = "preset.lua",
  files = { "three-strokes.txt", "missing.txt" },
  run = function(test)
    local panels = {}

    for _, window in ipairs(test.session().workspaces[1].windows) do
      addPanels(window, panels)
    end

    -- the missing file opens nothing
    local opened = {}

    for _, panel in ipairs(panels) do
      if panel.type == "sketch_c.canvas" and #panel.state.strokes > 0 then
        opened[#opened + 1] = panel.state
      end
    end

    test.match(opened, { { strokes = { { size = 4, points = { 10, 10, 50, 50 } }, { size = 6 }, { size = 2 } } } }, "opened canvases")
  end,
}
