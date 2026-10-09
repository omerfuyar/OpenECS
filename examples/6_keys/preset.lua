-- Run it with: OpenECS --preset examples/6_keys/preset.lua
---@type ecs.Preset
return {
  format = 1,
  name = "keys",
  version = "0.1.0",
  app = { id = "openecs.example.keys", name = "Keys" },
  depends = { stepper = "0.1" },
  pluginsDir = ".",
  -- a keys table: keys for the whole tool run any function by name; prefix adds keys after the core prefix (Alt+W),
  -- and a panel type's table changes its keys, winning over the plugin's defaults
  keys = {
    ["Ctrl+0"] = "stepper.resetAll",
    prefix = { Z = "stepper.resetAll" },
    ["stepper.panel"] = { ["Shift+Up"] = "stepper.up" },
  },
  workspaces = {
    { name = "Main",
      windows = {
        { split = "horizontal",
          { panels = { { type = "stepper.panel" } } },
          { panels = { { type = "stepper.panel" } } },
        },
      },
    },
  },
}
