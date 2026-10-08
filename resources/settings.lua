-- The core's own settings: OpenECS reads them at start, and they are the lowest settings layer.
-- Plugins' defaults, the preset, the settings window and the user's settings file override them.
return {
  -- the key combination before a core action
  ["ecs.prefix"] = "Alt+W",

  -- the keys after the prefix and the functions they run; other layers add to them, and false removes a key
  ["ecs.prefix_keys"] = {
    ["Left"] = "ecs.focus_left",
    ["Right"] = "ecs.focus_right",
    ["Up"] = "ecs.focus_up",
    ["Down"] = "ecs.focus_down",
    ["Shift+Left"] = "ecs.move_left",
    ["Shift+Right"] = "ecs.move_right",
    ["Shift+Up"] = "ecs.move_up",
    ["Shift+Down"] = "ecs.move_down",
    ["Tab"] = "ecs.next_tab",
    ["M"] = "ecs.maximize",
    ["X"] = "ecs.close",
    ["Shift+X"] = "ecs.close_group",
    ["L"] = "ecs.lock",
    ["T"] = "ecs.reopen",
    ["R"] = "ecs.restart",
    ["1"] = "ecs.workspace_1",
    ["2"] = "ecs.workspace_2",
    ["3"] = "ecs.workspace_3",
    ["4"] = "ecs.workspace_4",
    ["5"] = "ecs.workspace_5",
    ["6"] = "ecs.workspace_6",
    ["7"] = "ecs.workspace_7",
    ["8"] = "ecs.workspace_8",
    ["9"] = "ecs.workspace_9",
    ["0"] = "ecs.workspace_10",
  },

  -- how focus follows the pointer: "click" or "hover"
  ["ecs.focus"] = "click",

  -- frame rate in percent of the display's refresh rate: 100 waits for every refresh, 50 for every second one, 0 turns vsync off
  ["ecs.vsync"] = 100,
}
