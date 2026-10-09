-- files on the command line go to the preset's open function, here boxes.open, which opens a box with each file's lines

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
  preset = "presets/boxes.lua",
  files = { "files/three-lines.txt", "files/missing.txt" },
  run = function(test)
    local panels = {}

    for _, window in ipairs(test.session().workspaces[1].windows) do
      addPanels(window, panels)
    end

    -- the missing file opens nothing
    local opened = {}

    for _, panel in ipairs(panels) do
      if panel.type == "boxes.one" and #panel.state.lines > 0 then
        opened[#opened + 1] = panel.state
      end
    end

    test.match(opened, { { lines = { "first line", "second line", "third line" } } }, "opened boxes")
  end,
}
