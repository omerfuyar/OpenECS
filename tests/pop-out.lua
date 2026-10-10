-- pop-out windows: a preset's pop-out window, dragging a panel into another window, popping out with a key and by dragging a tab out of the window, closing one, and workspaces hiding them

-- the types of the panels of each OS window of the first workspace, in order
local function windows(test)
  local types = {}

  local function add(node, list)
    for _, panel in ipairs(node.panels or {}) do
      list[#list + 1] = panel.type
    end

    for _, child in ipairs(node) do
      add(child, list)
    end
  end

  for i, window in ipairs(test.session().workspaces[1].windows) do
    types[i] = {}
    add(window, types[i])
  end

  return types
end

return {
  preset = "presets/pop-out.lua",
  run = function(test)
    -- the preset's pop-out window has its size and its panel
    test.match(windows(test), { { "boxes.one", "boxes.two", "boxes.three" }, { "cboxes.box" } }, "the preset's windows")
    test.match(test.session().workspaces[1].windows[2], { width = 300, height = 200 }, "the pop-out window's size")
    test.match(test.rect(4), { window = 2, x = 0, y = 0, width = 300, height = 200 }, "the pop-out panel")

    -- a click in the pop-out window goes to its panel and focuses it; a panel alone in its window stays
    test.window(2)
    test.click(50, 50)
    test.match(test.session().workspaces[1].focus, 4, "the focus after a click in the pop-out window")
    test.call("ecs.layout.popOut")
    test.match(#windows(test), 2, "a panel alone in its window")

    -- the pop-out panel's grip, dragged into the main window, joins a group there, and its window closes
    local mainX, mainY = test.window(1)
    local popX, popY = test.window(2)
    test.move(150, 30)
    test.drag(150, 8, mainX + 960 - popX, mainY + 400 - popY)
    test.match(windows(test), { { "boxes.one", "boxes.two", "boxes.three", "cboxes.box" } }, "after dragging into the main window")

    -- the key pops the focused panel out of the main window
    test.window(1)
    local one = test.rect(1)
    test.click(one.x + 20, one.y + 20)
    test.key("Alt+W")
    test.key("P")
    test.match(windows(test), { { "boxes.two", "boxes.three", "cboxes.box" }, { "boxes.one" } }, "after the key")
    test.match(test.rect(1).window, 2, "the popped panel's window")

    -- a tab dragged out of every window pops out
    local group = test.rect(4)
    test.drag(group.x + 150, group.y - 13, -100, -100)
    test.match(windows(test), { { "boxes.two", "cboxes.box" }, { "boxes.one" }, { "boxes.three" } }, "after the drag")

    -- the close button of a pop-out window closes its panels
    test.window(test.rect(3).window)
    test.close()
    test.match(windows(test), { { "boxes.two", "cboxes.box" }, { "boxes.one" } }, "after closing a window")

    -- another workspace hides the pop-out windows, and shows them again when it is left
    test.call("ecs.workspace.switch2")
    assert(not pcall(test.rect, 1), "the pop-out panel is hidden in workspace 2")
    assert(not pcall(test.window, 2), "workspace 2 has no pop-out window")
    test.call("ecs.workspace.switch1")
    test.match(test.rect(1).window, 2, "the pop-out panel in workspace 1 again")
  end,
}
