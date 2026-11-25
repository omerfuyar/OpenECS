-- The core's own settings and keys: OpenECS reads them at start, and they are defaults, the lowest settings layer.
-- The preset and the user's settings file override them.
---@type ecs.SettingsFile
return {
  -- plugins that every tool loads: the settings window, the list of keys, the list of panel types to open, the list of plugins to enable or disable,
  -- the launcher, which goes to another tool, and the profiler, which shows where the time goes
  plugins = { "settings", "keys", "panels", "plugins", "launcher", "profiler" },

  -- the key combination before a core action
  ["ecs.prefix"] = "Alt+W",

  -- the core's keys: prefix holds the keys after the prefix and the functions they run (DESIGN 7.8);
  -- plugins give their own functions keys after the prefix too, below these, and presets and the user's settings add to them, and false removes a key
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
      ["Ctrl+R"] = "ecs.panel.reloadPlugin",
      ["Shift+R"] = "ecs.layout.reset",
      ["Q"] = "ecs.app.quit",
      ["S"] = "ecs.session.save",
      ["O"] = "ecs.session.open",
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
      ["Shift+1"] = "ecs.layout.moveToWorkspace1",
      ["Shift+2"] = "ecs.layout.moveToWorkspace2",
      ["Shift+3"] = "ecs.layout.moveToWorkspace3",
      ["Shift+4"] = "ecs.layout.moveToWorkspace4",
      ["Shift+5"] = "ecs.layout.moveToWorkspace5",
      ["Shift+6"] = "ecs.layout.moveToWorkspace6",
      ["Shift+7"] = "ecs.layout.moveToWorkspace7",
      ["Shift+8"] = "ecs.layout.moveToWorkspace8",
      ["Shift+9"] = "ecs.layout.moveToWorkspace9",
      ["Shift+0"] = "ecs.layout.moveToWorkspace10",
      ["Shift+N"] = "ecs.layout.moveToNewWorkspace",
      ["W"] = "ecs.workspace.menu",
      ["A"] = "ecs.app.menu",
    },
  },

  -- the menus of the title bar that OpenECS draws, after the tool's name, before the plugins' menu Tools; they are the tool's menu, which
  -- ecs.app.menu opens with keys; an entry is a function's name,
  -- or { "name", label = "Label", setting = "a bool setting" }, which shows the entry on while the setting is true;
  -- functions that no plugin of the tool registered are left out
  ["ecs.titleMenus"] = {
    { label = "File", "ecs.session.open", "ecs.session.save", "ecs.app.quit" },
    { label = "Window", "ecs.layout.reopen", "ecs.layout.maximize", "ecs.layout.popOut", "ecs.layout.lock", "ecs.layout.moveToNewWorkspace", "ecs.layout.reset" },
  },

  -- the tool's own buttons, after the menus, in the same form; a preset gives them
  ["ecs.titleButtons"] = {},

  -- save the tool's session when it quits, and start from it the next time; --fresh still starts from the preset
  ["ecs.keepSession"] = false,

  -- reload a plugin when its files change, checking them every interval in seconds, so a plugin's author sees a change without a restart;
  -- a native plugin loads from a copy of its library then, so its build can replace the library
  ["ecs.reloadPlugins"] = false,
  ["ecs.reloadInterval"] = 1,

  -- log the failures that services give their callers, such as a file that cannot be read; a tool that expects them turns this off
  ["ecs.logServiceErrors"] = true,

  -- how focus follows the pointer: "click" or "hover"
  ["ecs.focus"] = "click",

  -- measure where the time goes, by step of each frame, by plugin and by panel, for a profiler to show; it costs a little while on
  ["ecs.profile"] = false,

  -- frame rate in percent of the display's refresh rate: 100 waits for every refresh, 50 for every second one, 0 turns vsync off
  ["ecs.vsync"] = 100,

  -- the OS window when it opens, in layout units, and the core's font; a relative font path starts at the executable's folder;
  -- "resources/Roboto-Regular.ttf" ships too, for a proportional font
  ["ecs.windowWidth"] = 1280,
  ["ecs.windowHeight"] = 800,
  ["ecs.font"] = "resources/JetBrainsMono-Regular.ttf",
  ["ecs.fontSize"] = 13,

  -- how large panels draw what they hold: 1.25 makes their text and elements a quarter larger
  ["ecs.scale"] = 1,

  -- OpenECS draws the title bar of its OS windows, with buttons to minimize, maximize and close them, and edges to resize them;
  -- false leaves them to the system, which on some desktops draws none
  ["ecs.titleBar"] = true,
  ["ecs.titleBarHeight"] = 28,

  -- the colours of the interface: "system" follows the system's choice of dark or light, and "dark" or "light" choose one
  ["ecs.theme"] = "system",

  -- each theme gives the colours of the core's interface, "#RRGGBB" or "#RRGGBBAA" with opacity; a theme may set any setting,
  -- and the user's settings file may add to a theme; a colour set outside the themes wins over every theme
  themes = {
    dark = {
      ["ecs.colorBackground"] = "#1B1B1B",
      ["ecs.colorFrame"] = "#141414",
      ["ecs.colorTabRow"] = "#232323",
      ["ecs.colorTab"] = "#282828",
      ["ecs.colorTabShown"] = "#303030",
      ["ecs.colorPanel"] = "#303030",
      ["ecs.colorField"] = "#1E1E1E",
      ["ecs.colorButton"] = "#4A4A4A",
      ["ecs.colorButtonHover"] = "#575757",
      ["ecs.colorHover"] = "#3A3A3A",
      ["ecs.colorSelected"] = "#3D5F91",
      ["ecs.colorAccent"] = "#689EEA",
      ["ecs.colorText"] = "#E2E2E2",
      ["ecs.colorTextDim"] = "#9C9C9C",
      ["ecs.colorOverlay"] = "#1F1F1FF8",
      ["ecs.colorDrop"] = "#689EEA46",
      ["ecs.colorPlaceholder"] = "#3B2628",
      ["ecs.colorError"] = "#F07178",
      ["ecs.colorWarning"] = "#E5C07B",
    },
    light = {
      ["ecs.colorBackground"] = "#A3A3A3",
      ["ecs.colorFrame"] = "#8C8C8C",
      ["ecs.colorTabRow"] = "#BDBDBD",
      ["ecs.colorTab"] = "#C4C4C4",
      ["ecs.colorTabShown"] = "#D4D4D4",
      ["ecs.colorPanel"] = "#D4D4D4",
      ["ecs.colorField"] = "#F4F4F4",
      ["ecs.colorButton"] = "#E8E8E8",
      ["ecs.colorButtonHover"] = "#F6F6F6",
      ["ecs.colorHover"] = "#C6C6C6",
      ["ecs.colorSelected"] = "#A8C4EE",
      ["ecs.colorAccent"] = "#1F55B0",
      ["ecs.colorText"] = "#141414",
      ["ecs.colorTextDim"] = "#4F4F4F",
      ["ecs.colorOverlay"] = "#ECECECF8",
      ["ecs.colorDrop"] = "#1F55B046",
      ["ecs.colorPlaceholder"] = "#EBCFD2",
      ["ecs.colorError"] = "#A1231B",
      ["ecs.colorWarning"] = "#7A5200",
    },
  },

  -- sizes, in layout units; dividerSize is the gap between groups, and at the window's edges
  ["ecs.tabRowHeight"] = 24,
  ["ecs.dividerSize"] = 4,
  ["ecs.gripHeight"] = 20,

  -- distances, in layout units: a grip shows within gripZone of a panel's top edge; a press drags after moving dragThreshold;
  -- a dragged panel docks within dockEdge of the OS window's edge, and splits a panel within splitDepth of its edge
  ["ecs.gripZone"] = 24,
  ["ecs.dragThreshold"] = 6,
  ["ecs.dockEdge"] = 16,
  ["ecs.splitDepth"] = 80,

  -- how many closed panels ecs.layout.reopen remembers
  ["ecs.reopenLimit"] = 20,
}
