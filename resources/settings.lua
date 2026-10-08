-- The core's own settings: OpenECS reads them at start, and they are the lowest settings layer.
-- Plugins' defaults, the preset, the settings window and the user's settings file override them.
return {
  -- the key combination before a core action
  ["ecs.prefix"] = "Alt+W",

  -- the keys after the prefix and the functions they run; other layers add to them, and false removes a key
  ["ecs.prefixKeys"] = {
    ["Left"] = "ecs.layout.focusLeft",
    ["Right"] = "ecs.layout.focusRight",
    ["Up"] = "ecs.layout.focusUp",
    ["Down"] = "ecs.layout.focusDown",
    ["Shift+Left"] = "ecs.layout.moveLeft",
    ["Shift+Right"] = "ecs.layout.moveRight",
    ["Shift+Up"] = "ecs.layout.moveUp",
    ["Shift+Down"] = "ecs.layout.moveDown",
    ["Tab"] = "ecs.layout.nextTab",
    ["M"] = "ecs.layout.maximize",
    ["X"] = "ecs.layout.close",
    ["Shift+X"] = "ecs.layout.closeGroup",
    ["L"] = "ecs.layout.lock",
    ["T"] = "ecs.layout.reopen",
    ["R"] = "ecs.panel.restart",
    ["S"] = "ecs.session.save",
    ["O"] = "ecs.session.open",
    ["1"] = "ecs.workspace1",
    ["2"] = "ecs.workspace2",
    ["3"] = "ecs.workspace3",
    ["4"] = "ecs.workspace4",
    ["5"] = "ecs.workspace5",
    ["6"] = "ecs.workspace6",
    ["7"] = "ecs.workspace7",
    ["8"] = "ecs.workspace8",
    ["9"] = "ecs.workspace9",
    ["0"] = "ecs.workspace10",
  },

  -- how focus follows the pointer: "click" or "hover"
  ["ecs.focus"] = "click",

  -- frame rate in percent of the display's refresh rate: 100 waits for every refresh, 50 for every second one, 0 turns vsync off
  ["ecs.vsync"] = 100,

  -- the OS window when it opens, in layout units, and the core's font; a relative font path starts at the executable's folder
  ["ecs.windowWidth"] = 1280,
  ["ecs.windowHeight"] = 800,
  ["ecs.font"] = "resources/Roboto-Regular.ttf",
  ["ecs.fontSize"] = 14,

  -- colours of the core's interface: "#RRGGBB", or "#RRGGBBAA" with opacity
  ["ecs.colorBackground"] = "#18191C",
  ["ecs.colorTabRow"] = "#202226",
  ["ecs.colorTab"] = "#282B30",
  ["ecs.colorTabShown"] = "#3A3E46",
  ["ecs.colorText"] = "#DCDEE2",
  ["ecs.colorTextDim"] = "#969AA0",
  ["ecs.colorAccent"] = "#4C8BF5",
  ["ecs.colorPlaceholder"] = "#2C1E22",
  ["ecs.colorOverlay"] = "#1C1E22F5",
  ["ecs.colorDrop"] = "#4C8BF546",
  ["ecs.colorSelected"] = "#3A3E46",

  -- sizes, in layout units
  ["ecs.tabRowHeight"] = 26,
  ["ecs.dividerSize"] = 4,
  ["ecs.gripHeight"] = 20,

  -- distances, in layout units: a grip shows within gripZone of a panel's top edge; a press drags after moving dragThreshold;
  -- a dragged panel docks within dockEdge of the OS window's edge, and splits a panel within splitDepth of its edge;
  -- a wheel step scrolls a tab row by tabScrollStep
  ["ecs.gripZone"] = 24,
  ["ecs.dragThreshold"] = 6,
  ["ecs.dockEdge"] = 16,
  ["ecs.splitDepth"] = 80,
  ["ecs.tabScrollStep"] = 40,

  -- how many closed panels ecs.layout.reopen remembers
  ["ecs.reopenLimit"] = 20,
}
