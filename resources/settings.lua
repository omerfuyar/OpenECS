-- The core's own settings and keys: OpenECS reads them at start, and they are defaults, the lowest settings layer.
-- The preset and the user's settings file override them.
---@type ecs.SettingsFile
return {
  -- plugins that every tool loads: the settings window, the list of panel types to open, and the launcher, which goes to another tool
  plugins = { "settings", "panels", "launcher" },

  -- the key combination before a core action
  ["ecs.prefix"] = "Alt+W",

  -- the core's keys: prefix holds the keys after the prefix and the functions they run (DESIGN 7.8)
  -- presets and the user's settings add to them, and false removes a key
  keys = {
    prefix = {
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
      ["P"] = "ecs.layout.popOut",
      ["X"] = "ecs.layout.close",
      ["Shift+X"] = "ecs.layout.closeGroup",
      ["L"] = "ecs.layout.lock",
      ["T"] = "ecs.layout.reopen",
      ["R"] = "ecs.panel.restart",
      ["Shift+R"] = "ecs.layout.reset",
      ["Q"] = "ecs.app.quit",
      ["S"] = "ecs.session.save",
      ["O"] = "ecs.session.open",
      ["/"] = "launcher.open",
      [","] = "settings.open",
      ["."] = "panels.open",
      ["1"] = "ecs.workspace.switch1",
      ["2"] = "ecs.workspace.switch2",
      ["3"] = "ecs.workspace.switch3",
      ["4"] = "ecs.workspace.switch4",
      ["5"] = "ecs.workspace.switch5",
      ["6"] = "ecs.workspace.switch6",
      ["7"] = "ecs.workspace.switch7",
      ["8"] = "ecs.workspace.switch8",
      ["9"] = "ecs.workspace.switch9",
      ["0"] = "ecs.workspace.switch10",
      ["W"] = "ecs.workspace.menu",
    },
  },

  -- save the tool's session when it quits, and start from it the next time; --fresh still starts from the preset
  ["ecs.keepSession"] = false,

  -- how focus follows the pointer: "click" or "hover"
  ["ecs.focus"] = "click",

  -- frame rate in percent of the display's refresh rate: 100 waits for every refresh, 50 for every second one, 0 turns vsync off
  ["ecs.vsync"] = 100,

  -- the OS window when it opens, in layout units, and the core's font; a relative font path starts at the executable's folder
  ["ecs.windowWidth"] = 1280,
  ["ecs.windowHeight"] = 800,
  ["ecs.font"] = "resources/Roboto-Regular.ttf",
  ["ecs.fontSize"] = 14,

  -- OpenECS draws the title bar of its OS windows, with buttons to minimize, maximize and close them, and edges to resize them;
  -- false leaves them to the system, which on some desktops draws none
  ["ecs.titleBar"] = true,
  ["ecs.titleBarHeight"] = 30,

  -- the colours of the interface: "system" follows the system's choice of dark or light, and "dark" or "light" choose one
  ["ecs.theme"] = "system",

  -- each theme gives the colours of the core's interface, "#RRGGBB" or "#RRGGBBAA" with opacity; a theme may set any setting,
  -- and the user's settings file may add to a theme; a colour set outside the themes wins over every theme
  themes = {
    dark = {
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
    },
    light = {
      ["ecs.colorBackground"] = "#D9DCE1",
      ["ecs.colorTabRow"] = "#E6E8EC",
      ["ecs.colorTab"] = "#DADDE2",
      ["ecs.colorTabShown"] = "#FFFFFF",
      ["ecs.colorText"] = "#1F2328",
      ["ecs.colorTextDim"] = "#5C626B",
      ["ecs.colorAccent"] = "#2F6FE0",
      ["ecs.colorPlaceholder"] = "#F3DFE2",
      ["ecs.colorOverlay"] = "#FBFBFCF5",
      ["ecs.colorDrop"] = "#2F6FE046",
      ["ecs.colorSelected"] = "#D3DAE6",
    },
  },

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
