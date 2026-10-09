-- Run it with: OpenECS --preset examples/6_keys/preset.lua
return {
  format = 1,
  name = "keys",
  version = "0.1.0",
  app = { id = "openecs.example.keys", name = "Keys" },
  depends = { stepper = "0.1" },
  pluginsDir = ".",
  keys = { ["Ctrl+0"] = "stepper.resetAll" }, -- a preset binds keys for the whole tool, to any function by name
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
