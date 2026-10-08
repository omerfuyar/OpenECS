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

  -- the OS window when it opens, in layout units, and the core's font; a relative font path starts at the executable's folder
  ["ecs.window_width"] = 1280,
  ["ecs.window_height"] = 800,
  ["ecs.font"] = "resources/Roboto-Regular.ttf",
  ["ecs.font_size"] = 14,

  -- colours of the core's interface: "#RRGGBB", or "#RRGGBBAA" with opacity
  ["ecs.color_background"] = "#18191C",
  ["ecs.color_tab_row"] = "#202226",
  ["ecs.color_tab"] = "#282B30",
  ["ecs.color_tab_shown"] = "#3A3E46",
  ["ecs.color_text"] = "#DCDEE2",
  ["ecs.color_text_dim"] = "#969AA0",
  ["ecs.color_accent"] = "#4C8BF5",
  ["ecs.color_placeholder"] = "#2C1E22",
  ["ecs.color_overlay"] = "#1C1E22F5",
  ["ecs.color_drop"] = "#4C8BF546",
  ["ecs.color_selected"] = "#3A3E46",

  -- sizes, in layout units
  ["ecs.tab_row_height"] = 26,
  ["ecs.divider_size"] = 4,
  ["ecs.grip_height"] = 20,

  -- distances, in layout units: a grip shows within grip_zone of a panel's top edge; a press drags after moving drag_threshold;
  -- a dragged panel docks within dock_edge of the OS window's edge, and splits a panel within split_depth of its edge;
  -- a wheel step scrolls a tab row by tab_scroll_step
  ["ecs.grip_zone"] = 24,
  ["ecs.drag_threshold"] = 6,
  ["ecs.dock_edge"] = 16,
  ["ecs.split_depth"] = 80,
  ["ecs.tab_scroll_step"] = 40,

  -- how many closed panels ecs.reopen remembers
  ["ecs.reopen_limit"] = 20,
}
